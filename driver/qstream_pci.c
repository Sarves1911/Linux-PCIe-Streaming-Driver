#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include "../protocol/qstream_wire.h"

static const struct pci_device_id qstream_pci_ids[] = {
    { PCI_DEVICE(QSTREAM_PCI_VENDOR_ID, QSTREAM_PCI_DEVICE_ID) },
    { 0 }
};

MODULE_DEVICE_TABLE(pci, qstream_pci_ids);


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

    return 0;
}


static void qstream_remove(struct pci_dev *pdev)
{
    void __iomem *bar0 = pci_get_drvdata(pdev);   
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