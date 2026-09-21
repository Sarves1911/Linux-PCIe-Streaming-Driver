#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include "../protocol/qstream_wire.h"
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include "../include/uapi/qstream_ioctl.h"
#include <linux/poll.h>
#include <linux/vmalloc.h>
#include "../include/uapi/qstream_ring.h"
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#define QSTREAM_IRQ_DRAIN_LIMIT 256u

static const struct pci_device_id qstream_pci_ids[] = {
    { PCI_DEVICE(QSTREAM_PCI_VENDOR_ID, QSTREAM_PCI_DEVICE_ID) },
    { 0 }
};


struct qstream_device {
    struct pci_dev *pdev;
    void __iomem *bar0;
    struct qstream_shared_ring *shared;
    u64 head;
    u64 tail;
    u64 dropped;
    struct miscdevice miscdev;
    wait_queue_head_t read_queue;
    spinlock_t ring_lock;
    atomic_t is_open;
    u32 generation;
};

static void qstream_free_shared_ring(void *data)
{
    vfree(data);
}

static long qstream_ioctl(struct file *file,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct miscdevice *miscdev = file->private_data;
    struct qstream_device *qdev =
        container_of(miscdev, struct qstream_device, miscdev);

    switch (cmd) {
    case QSTREAM_IOCTL_START:
        iowrite32(QSTREAM_CONTROL_START,
                  qdev->bar0 + QSTREAM_REG_CONTROL);

        dev_info(&qdev->pdev->dev,
                 "qstream: START requested by userspace\n");
        return 0;

    case QSTREAM_IOCTL_STOP:
        iowrite32(QSTREAM_CONTROL_STOP,
                qdev->bar0 + QSTREAM_REG_CONTROL);
        synchronize_irq(qdev->pdev->irq);

        dev_info(&qdev->pdev->dev,
                "qstream: STOP requested by userspace\n");
        return 0;
    case QSTREAM_IOCTL_CONSUME: {
        __u32 count;
        u64 available;
        unsigned long lock_flags;

        if (copy_from_user(&count,
                        (void __user *)arg,
                        sizeof(count)))
            return -EFAULT;

        if (count == 0)
            return 0;

        spin_lock_irqsave(&qdev->ring_lock, lock_flags);

        available = qdev->head - qdev->tail;

        if (count > available) {
            spin_unlock_irqrestore(&qdev->ring_lock,
                                lock_flags);
            return -EINVAL;
        }

        qdev->tail += count;

        smp_store_release(&qdev->shared->header.tail,
                        qdev->tail);

        spin_unlock_irqrestore(&qdev->ring_lock,
                            lock_flags);

        return 0;
    case QSTREAM_IOCTL_RESET: {
        unsigned long lock_flags;
        u32 new_generation;

        iowrite32(QSTREAM_CONTROL_RESET,
                qdev->bar0 + QSTREAM_REG_CONTROL);

        new_generation =
            ioread32(qdev->bar0 + QSTREAM_REG_GENERATION);

        synchronize_irq(qdev->pdev->irq);

        spin_lock_irqsave(&qdev->ring_lock, lock_flags);

        qdev->head = 0;
        qdev->tail = 0;
        qdev->dropped = 0;
        qdev->generation = new_generation;

        WRITE_ONCE(qdev->shared->header.head, 0);
        WRITE_ONCE(qdev->shared->header.tail, 0);
        WRITE_ONCE(qdev->shared->header.dropped, 0);

        smp_store_release(
            &qdev->shared->header.generation,
            qdev->generation);

        spin_unlock_irqrestore(&qdev->ring_lock,
                            lock_flags);

        dev_info(&qdev->pdev->dev,
                "qstream: RESET completed, generation=%u\n",
                qdev->generation);

        return 0;
    }
    }
    default:
        return -ENOTTY;
    }
}

static __poll_t qstream_poll(struct file *file, poll_table *wait)
{
    struct miscdevice *miscdev = file->private_data;
    struct qstream_device *qdev =
        container_of(miscdev, struct qstream_device, miscdev);
    unsigned long lock_flags;
    bool record_available;

    poll_wait(file, &qdev->read_queue, wait);

    spin_lock_irqsave(&qdev->ring_lock, lock_flags);

    record_available = qdev->head != qdev->tail;

    spin_unlock_irqrestore(&qdev->ring_lock,
                           lock_flags);

    if (record_available)
        return EPOLLIN | EPOLLRDNORM;

    return 0;
}

static int qstream_mmap(struct file *file,
                        struct vm_area_struct *vma)
{
    struct miscdevice *miscdev = file->private_data;
    struct qstream_device *qdev =
        container_of(miscdev, struct qstream_device, miscdev);
    unsigned long requested_size =
        vma->vm_end - vma->vm_start;

    if (vma->vm_pgoff != 0)
        return -EINVAL;

    if (requested_size != QSTREAM_RING_MMAP_SIZE)
        return -EINVAL;

    if (vma->vm_flags & VM_WRITE)
        return -EPERM;

    vm_flags_clear(vma, VM_MAYWRITE);
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    return remap_vmalloc_range(vma, qdev->shared, 0);
}

static int qstream_open(struct inode *inode,
                        struct file *file)
{
    struct miscdevice *miscdev = file->private_data;
    struct qstream_device *qdev =
        container_of(miscdev,
                     struct qstream_device,
                     miscdev);
    int ret;

    if (atomic_cmpxchg(&qdev->is_open, 0, 1) != 0)
        return -EBUSY;

    ret = nonseekable_open(inode, file);
    if (ret)
        atomic_set(&qdev->is_open, 0);

    return ret;
}

static int qstream_release(struct inode *inode,
                           struct file *file)
{
    struct miscdevice *miscdev = file->private_data;
    struct qstream_device *qdev =
        container_of(miscdev,
                     struct qstream_device,
                     miscdev);

    iowrite32(QSTREAM_CONTROL_STOP,
              qdev->bar0 + QSTREAM_REG_CONTROL);

    synchronize_irq(qdev->pdev->irq);

    atomic_set(&qdev->is_open, 0);

    return 0;
}

static const struct file_operations qstream_fops = {
    .owner = THIS_MODULE,
    .open = qstream_open,
    .release = qstream_release,
    .unlocked_ioctl = qstream_ioctl,
    .poll = qstream_poll,
    .mmap = qstream_mmap,
    .llseek = no_llseek,
};
MODULE_DEVICE_TABLE(pci, qstream_pci_ids);

// static irqreturn_t qstream_irq_handler(int irq, void *data)
// {
//     struct pci_dev *pdev = data;
//     struct qstream_device *qdev = pci_get_drvdata(pdev);
//     void __iomem *bar0 = qdev->bar0;
//     struct qstream_record record;
//     u32 status;
//     u32 fifo_count;
//     u32 checksum = 0;
//     unsigned int i;
//     unsigned long lock_flags;
//     bool stored = false;

//     status = ioread32(bar0 + QSTREAM_REG_IRQ_STATUS);

//     if (!status)
//         return IRQ_NONE;

//     fifo_count = ioread32(bar0 + QSTREAM_REG_FIFO_COUNT);

//     if (!fifo_count) {
//         iowrite32(status, bar0 + QSTREAM_REG_IRQ_ACK);
//         return IRQ_HANDLED;
//     }

//     for (i = 0; i < QSTREAM_RECORD_WORDS; i++) {
//         record.words[i] =
//             ioread32(bar0 + QSTREAM_REG_DATA_WORD(i));
//     }

//     for (i = 0; i < QSTREAM_WORD_CHECKSUM; i++)
//         checksum ^= record.words[i];

//     if (checksum != record.words[QSTREAM_WORD_CHECKSUM]) {
//         dev_err_ratelimited(&pdev->dev,
//                             "qstream: checksum mismatch\n");
//         goto consume_record;
//     }

//     spin_lock_irqsave(&qdev->ring_lock, lock_flags);

//     if (qdev->head - qdev->tail >= QSTREAM_RING_CAPACITY) {
//         qdev->dropped++;

//         WRITE_ONCE(qdev->shared->header.dropped,
//                 qdev->dropped);
//     } else {
//         qdev->shared->records[qdev->head &
//                             QSTREAM_RING_MASK] = record;

//         qdev->head++;

//         smp_store_release(&qdev->shared->header.head,
//                         qdev->head);

//         stored = true;
//     }

//     spin_unlock_irqrestore(&qdev->ring_lock, lock_flags);

//     if (stored)
//         wake_up_interruptible(&qdev->read_queue);
//     consume_record:
//         iowrite32(QSTREAM_FIFO_POP_ONE,
//                 bar0 + QSTREAM_REG_FIFO_POP);

//         iowrite32(status,
//                 bar0 + QSTREAM_REG_IRQ_ACK);

//         return IRQ_HANDLED;
// }

static irqreturn_t qstream_irq_handler(int irq, void *data)
{
    struct pci_dev *pdev = data;
    struct qstream_device *qdev = pci_get_drvdata(pdev);
    void __iomem *bar0 = qdev->bar0;
    struct qstream_record record;
    unsigned long lock_flags;
    unsigned int drained;
    unsigned int i;
    u32 fifo_count;
    u32 checksum;
    u32 status;
    bool wake_runtime = false;

    status = ioread32(bar0 + QSTREAM_REG_IRQ_STATUS);

    if (!status)
        return IRQ_NONE;

    for (drained = 0;
         drained < QSTREAM_IRQ_DRAIN_LIMIT;
         drained++) {
        fifo_count =
            ioread32(bar0 + QSTREAM_REG_FIFO_COUNT);

        if (!fifo_count)
            break;

        checksum = 0;

        for (i = 0; i < QSTREAM_RECORD_WORDS; i++) {
            record.words[i] =
                ioread32(
                    bar0 + QSTREAM_REG_DATA_WORD(i));
        }

        for (i = 0; i < QSTREAM_WORD_CHECKSUM; i++)
            checksum ^= record.words[i];

        if (checksum !=
    record.words[QSTREAM_WORD_CHECKSUM]) {
        dev_err_ratelimited(
            &pdev->dev,
            "qstream: checksum mismatch\n");
    } else if (record.words[QSTREAM_WORD_GENERATION] !=
            READ_ONCE(qdev->generation)) {
        dev_warn_ratelimited(
            &pdev->dev,
            "qstream: discarded stale generation %u, "
            "expected %u\n",
            record.words[QSTREAM_WORD_GENERATION],
            READ_ONCE(qdev->generation));
    } else {
        spin_lock_irqsave(&qdev->ring_lock,
                        lock_flags);
            if (qdev->head - qdev->tail >=
                QSTREAM_RING_CAPACITY) {
                qdev->dropped++;

                WRITE_ONCE(
                    qdev->shared->header.dropped,
                    qdev->dropped);
            } else {
                qdev->shared->records[
                    qdev->head &
                    QSTREAM_RING_MASK] = record;

                qdev->head++;

                smp_store_release(
                    &qdev->shared->header.head,
                    qdev->head);

                wake_runtime = true;
            }

            spin_unlock_irqrestore(
                &qdev->ring_lock,
                lock_flags);
        }

        iowrite32(QSTREAM_FIFO_POP_ONE,
                  bar0 + QSTREAM_REG_FIFO_POP);
    }

    iowrite32(status,
              bar0 + QSTREAM_REG_IRQ_ACK);

    if (wake_runtime)
        wake_up_interruptible(&qdev->read_queue);

    return IRQ_HANDLED;
}

static int qstream_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    void __iomem *bar0;
    u32 magic;
    u32 version;
    u32 scratch_value = 0x12345678;
    u32 scratch_response;
    int ret;
    //u32 stream_status;
    struct qstream_device *qdev;

    qdev = devm_kzalloc(&pdev->dev, sizeof(*qdev), GFP_KERNEL);
    if (!qdev)
        return -ENOMEM;

    qdev->shared = vmalloc_user(QSTREAM_RING_MMAP_SIZE);
    if (!qdev->shared)
        return -ENOMEM;

    ret = devm_add_action_or_reset(&pdev->dev,
                                qstream_free_shared_ring,
                                qdev->shared);
    if (ret)
        return ret;

    qdev->pdev = pdev;
    init_waitqueue_head(&qdev->read_queue);
    spin_lock_init(&qdev->ring_lock);
    atomic_set(&qdev->is_open, 0);
    qdev->shared->header.magic = QSTREAM_RING_MAGIC;
    qdev->shared->header.version = QSTREAM_RING_VERSION;
    qdev->shared->header.capacity = QSTREAM_RING_CAPACITY;
    qdev->shared->header.record_size =
        sizeof(struct qstream_record);
    
        

    ret = pci_enable_device(pdev);
    if (ret) {
        dev_err(&pdev->dev, "qstream: failed to enable device: %d\n",
                ret);
        return ret;
    }

    ret = pci_request_region(pdev, 0, "qstream_pci");
    if (ret) {
        dev_err(&pdev->dev, "qstream: failed to reserve BAR0: %d\n",
                ret);
        pci_disable_device(pdev);
        return ret;
    }

    bar0 = pci_iomap(pdev, 0, 0);
    if (!bar0) {
        dev_err(&pdev->dev, "qstream: failed to map BAR0\n");
        pci_release_region(pdev, 0);
        pci_disable_device(pdev);
        return -ENOMEM;
    }

    qdev->bar0 = bar0;
    pci_set_drvdata(pdev, qdev);

    magic = ioread32(bar0 + QSTREAM_REG_MAGIC);
    version = ioread32(bar0 + QSTREAM_REG_VERSION);

    qdev->generation =
    ioread32(bar0 + QSTREAM_REG_GENERATION);

    WRITE_ONCE(qdev->shared->header.generation,
           qdev->generation);

    iowrite32(scratch_value, bar0 + QSTREAM_REG_SCRATCH);
    scratch_response = ioread32(bar0 + QSTREAM_REG_SCRATCH);

    dev_info(&pdev->dev,
         "qstream: magic=0x%08x version=0x%08x "
         "scratch=0x%08x generation=%u\n",
         magic,
         version,
         scratch_response,
         qdev->generation);

    ret = request_irq(pdev->irq,
                  qstream_irq_handler,
                  IRQF_SHARED,
                  "qstream_pci",
                  pdev);

    if (ret) {
        dev_err(&pdev->dev,
                "qstream: failed to register IRQ: %d\n",
                ret);

        pci_iounmap(pdev, bar0);
        pci_release_region(pdev, 0);
        pci_disable_device(pdev);
        return ret;
    }

    dev_info(&pdev->dev,
            "qstream: IRQ %d registered\n",
            pdev->irq);
    
    qdev->miscdev.minor = MISC_DYNAMIC_MINOR;
    qdev->miscdev.name = "qstream0";
    qdev->miscdev.fops = &qstream_fops;
    qdev->miscdev.parent = &pdev->dev;
    qdev->miscdev.mode = 0660;

    ret = misc_register(&qdev->miscdev);
    if (ret) {
        dev_err(&pdev->dev,
                "qstream: failed to register /dev/qstream0: %d\n",
                ret);

        free_irq(pdev->irq, pdev);
        pci_iounmap(pdev, bar0);
        pci_release_region(pdev, 0);
        pci_disable_device(pdev);
        return ret;
    }

    dev_info(&pdev->dev, "qstream: registered /dev/qstream0\n");
    
    // iowrite32(QSTREAM_CONTROL_START,
    //       bar0 + QSTREAM_REG_CONTROL);

    // stream_status =
    //     ioread32(bar0 + QSTREAM_REG_STATUS);

    // dev_info(&pdev->dev,
    //         "qstream: streaming started, status=0x%08x\n",
    //         stream_status);

    return 0;
}

static void qstream_remove(struct pci_dev *pdev)
{
    struct qstream_device *qdev = pci_get_drvdata(pdev);
    void __iomem *bar0 = qdev->bar0;

    iowrite32(QSTREAM_CONTROL_STOP,
              bar0 + QSTREAM_REG_CONTROL);

    iowrite32(~0u,
              bar0 + QSTREAM_REG_IRQ_ACK);

    misc_deregister(&qdev->miscdev);

    free_irq(pdev->irq, pdev);
    dev_info(&pdev->dev,
         "qstream: ring stored=%llu pending=%llu dropped=%llu\n",
         (unsigned long long)qdev->head,
         (unsigned long long)(qdev->head - qdev->tail),
         (unsigned long long)qdev->dropped);

    pci_iounmap(pdev, bar0);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);

    dev_info(&pdev->dev, "qstream: device removed\n");
}

static struct pci_driver qstream_pci_driver = {
    .name = "qstream_pci",
    .id_table = qstream_pci_ids,
    .probe = qstream_probe,
    .remove = qstream_remove,
};

module_pci_driver(qstream_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal PCI driver for QEMU qstream device");