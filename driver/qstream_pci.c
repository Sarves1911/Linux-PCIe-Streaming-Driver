#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include "../protocol/qstream_wire.h"
#include <linux/interrupt.h>

static const struct pci_device_id qstream_pci_ids[] = {
    { PCI_DEVICE(QSTREAM_PCI_VENDOR_ID, QSTREAM_PCI_DEVICE_ID) },
    { 0 }
};

MODULE_DEVICE_TABLE(pci, qstream_pci_ids);

static irqreturn_t qstream_irq_handler(int irq, void *data)
{
    struct pci_dev *pdev = data;
    void __iomem *bar0 = pci_get_drvdata(pdev);
    u32 words[QSTREAM_RECORD_WORDS];
    u32 status;
    u32 count;
    u32 checksum = 0;
    u64 sequence;
    u64 timestamp;
    unsigned int word;

    status = ioread32(bar0 + QSTREAM_REG_IRQ_STATUS);

    if (!status)
        return IRQ_NONE;

    count = ioread32(bar0 + QSTREAM_REG_FIFO_COUNT);

    if (!count) {
        dev_warn(&pdev->dev,
                 "qstream: DATA_READY with empty FIFO\n");

        iowrite32(status, bar0 + QSTREAM_REG_IRQ_ACK);
        return IRQ_HANDLED;
    }

    for (word = 0; word < QSTREAM_RECORD_WORDS; word++) {
        words[word] =
            ioread32(bar0 + QSTREAM_REG_DATA_WORD(word));
    }

    for (word = 0; word < QSTREAM_WORD_CHECKSUM; word++)
        checksum ^= words[word];

    sequence =
        ((u64)words[QSTREAM_WORD_SEQUENCE_HI] << 32) |
        words[QSTREAM_WORD_SEQUENCE_LO];

    timestamp =
        ((u64)words[QSTREAM_WORD_TIMESTAMP_HI] << 32) |
        words[QSTREAM_WORD_TIMESTAMP_LO];

    if (checksum != words[QSTREAM_WORD_CHECKSUM]) {
        dev_err(&pdev->dev,
                "qstream: sequence=%llu checksum mismatch\n",
                (unsigned long long)sequence);
    } else {
        dev_info(&pdev->dev,
                 "qstream: record sequence=%llu timestamp=%llu "
                 "value=0x%08x generation=%u checksum OK\n",
                 (unsigned long long)sequence,
                 (unsigned long long)timestamp,
                 words[QSTREAM_WORD_VALUE],
                 words[QSTREAM_WORD_GENERATION]);
    }

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

    pci_set_drvdata(pdev, bar0);

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
/* Temporary test: ask the card to generate one real record. */
    iowrite32(QSTREAM_CONTROL_GENERATE_ONE,
            bar0 + QSTREAM_REG_CONTROL);

    return 0;
}


static void qstream_remove(struct pci_dev *pdev)
{
    void __iomem *bar0 = pci_get_drvdata(pdev);   
    pci_iounmap(pdev, bar0);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);
    free_irq(pdev->irq, pdev);

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