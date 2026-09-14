#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include "../protocol/qstream_wire.h"
#include <linux/interrupt.h>
#define QSTREAM_RING_CAPACITY 1024u
#define QSTREAM_RING_MASK (QSTREAM_RING_CAPACITY - 1u)
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include "../include/uapi/qstream_ioctl.h"

static const struct pci_device_id qstream_pci_ids[] = {
    { PCI_DEVICE(QSTREAM_PCI_VENDOR_ID, QSTREAM_PCI_DEVICE_ID) },
    { 0 }
};

struct qstream_record {
    u32 words[QSTREAM_RECORD_WORDS];
};

struct qstream_device {
    struct pci_dev *pdev;
    void __iomem *bar0;

    struct qstream_record *ring;

    u64 head;
    u64 tail;
    u64 dropped;
    struct miscdevice miscdev;
};
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

        dev_info(&qdev->pdev->dev,
                 "qstream: STOP requested by userspace\n");
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations qstream_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = qstream_ioctl,
};

MODULE_DEVICE_TABLE(pci, qstream_pci_ids);

static irqreturn_t qstream_irq_handler(int irq, void *data)
{
    struct pci_dev *pdev = data;
    struct qstream_device *qdev = pci_get_drvdata(pdev);
    void __iomem *bar0 = qdev->bar0;
    struct qstream_record record;
    u32 status;
    u32 fifo_count;
    u32 checksum = 0;
    unsigned int i;

    status = ioread32(bar0 + QSTREAM_REG_IRQ_STATUS);

    if (!status)
        return IRQ_NONE;

    fifo_count = ioread32(bar0 + QSTREAM_REG_FIFO_COUNT);

    if (!fifo_count) {
        iowrite32(status, bar0 + QSTREAM_REG_IRQ_ACK);
        return IRQ_HANDLED;
    }

    for (i = 0; i < QSTREAM_RECORD_WORDS; i++) {
        record.words[i] =
            ioread32(bar0 + QSTREAM_REG_DATA_WORD(i));
    }

    for (i = 0; i < QSTREAM_WORD_CHECKSUM; i++)
        checksum ^= record.words[i];

    if (checksum != record.words[QSTREAM_WORD_CHECKSUM]) {
        dev_err_ratelimited(&pdev->dev,
                            "qstream: checksum mismatch\n");
        goto consume_record;
    }

    if (qdev->head - qdev->tail >= QSTREAM_RING_CAPACITY) {
        qdev->dropped++;
    } else {
        qdev->ring[qdev->head & QSTREAM_RING_MASK] = record;
        qdev->head++;
    }

consume_record:
    iowrite32(QSTREAM_FIFO_POP_ONE,
              bar0 + QSTREAM_REG_FIFO_POP);

    iowrite32(status,
              bar0 + QSTREAM_REG_IRQ_ACK);

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
    u32 stream_status;
    struct qstream_device *qdev;

    qdev = devm_kzalloc(&pdev->dev, sizeof(*qdev), GFP_KERNEL);
    if (!qdev)
        return -ENOMEM;

    qdev->ring = devm_kcalloc(&pdev->dev,
                            QSTREAM_RING_CAPACITY,
                            sizeof(*qdev->ring),
                            GFP_KERNEL);
    if (!qdev->ring)
        return -ENOMEM;

    qdev->pdev = pdev;

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

    iowrite32(scratch_value, bar0 + QSTREAM_REG_SCRATCH);
    scratch_response = ioread32(bar0 + QSTREAM_REG_SCRATCH);

    dev_info(&pdev->dev,
            "qstream: magic=0x%08x version=0x%08x scratch=0x%08x\n",
            magic, version, scratch_response);

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