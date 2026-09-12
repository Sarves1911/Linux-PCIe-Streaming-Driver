#include <linux/module.h>
#include <linux/pci.h>

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8
static const struct pci_device_id qstream_pci_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0 }
};

MODULE_DEVICE_TABLE(pci, qstream_pci_ids);

static int qstream_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    dev_info(&pdev->dev, "qstream: EDU device found\n");
    return 0;
}
static void qstream_remove(struct pci_dev *pdev)
{
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
