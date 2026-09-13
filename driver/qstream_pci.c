#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#define EDU_REG_ID 0x00
#define EDU_REG_LIVENESS 0x04
#define EDU_REG_IRQ_STATUS 0x24
#define EDU_REG_IRQ_RAISE  0x60
#define EDU_REG_IRQ_ACK    0x64
#define EDU_TEST_IRQ       0x01

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8
static const struct pci_device_id qstream_pci_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0 }
};

MODULE_DEVICE_TABLE(pci, qstream_pci_ids);

static irqreturn_t qstream_irq_handler(int irq, void *data)
{
    struct pci_dev *pdev = data;
    void __iomem *bar0 = pci_get_drvdata(pdev);
    u32 status;

    status = ioread32(bar0 + EDU_REG_IRQ_STATUS);

    if (!status)
        return IRQ_NONE;

    iowrite32(status, bar0 + EDU_REG_IRQ_ACK);

    dev_info(&pdev->dev,
             "qstream: interrupt handled, status=0x%08x\n",
             status);

    return IRQ_HANDLED;
}

static int qstream_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    void __iomem *bar0;
    u32 edu_id;
    int ret;
    u32 test_value = 0x12345678;
    u32 response;

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

    edu_id = ioread32(bar0 + EDU_REG_ID);

    dev_info(&pdev->dev, "qstream: EDU ID register = 0x%08x\n",
             edu_id);
    
    iowrite32(test_value, bar0 + EDU_REG_LIVENESS);
    response = ioread32(bar0 + EDU_REG_LIVENESS);

    dev_info(&pdev->dev,
         "qstream: liveness wrote 0x%08x, read 0x%08x\n",
         test_value, response);

    ret = request_irq(pdev->irq,
                  qstream_irq_handler,
                  IRQF_SHARED,
                  "qstream_pci",
                  pdev);

    if (ret) {
        dev_err(&pdev->dev, "qstream: failed to register IRQ: %d\n",
                ret);
        pci_iounmap(pdev, bar0);
        pci_release_region(pdev, 0);
        pci_disable_device(pdev);
        return ret;
    }

    dev_info(&pdev->dev, "qstream: IRQ %d registered\n", pdev->irq);

    /* Temporary test: ask EDU to raise interrupt bit 0. */
    iowrite32(EDU_TEST_IRQ, bar0 + EDU_REG_IRQ_RAISE);

    return 0;
}


static void qstream_remove(struct pci_dev *pdev)
{
    void __iomem *bar0 = pci_get_drvdata(pdev);
    free_irq(pdev->irq, pdev);
   
    pci_iounmap(pdev, bar0);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);

    dev_info(&pdev->dev, "qstream: EDU device removed\n");
}
static struct pci_driver qstream_pci_driver = {
    .name = "qstream_pci",
    .id_table = qstream_pci_ids,
    .probe = qstream_probe,
    .remove = qstream_remove,
};

module_pci_driver(qstream_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal PCI driver tested with QEMU EDU");
