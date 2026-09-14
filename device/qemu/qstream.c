#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"
#include "qstream_wire.h"
#include "qemu/module.h"
#define TYPE_QSTREAM_DEVICE "qstream"

OBJECT_DECLARE_SIMPLE_TYPE(QStreamState, QSTREAM_DEVICE)

struct QStreamState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    uint32_t scratch;
    uint32_t irq_status;
};
static void qstream_update_irq(QStreamState *s)
{
    if (s->irq_status)
        pci_set_irq(&s->parent_obj, 1);
    else
        pci_set_irq(&s->parent_obj, 0);
}

static void qstream_raise_irq(QStreamState *s, uint32_t bits)
{
    s->irq_status |= bits;
    qstream_update_irq(s);
}

static void qstream_ack_irq(QStreamState *s, uint32_t bits)
{
    s->irq_status &= ~bits;
    qstream_update_irq(s);
}

static uint64_t qstream_mmio_read(void *opaque,
                                  hwaddr addr,
                                  unsigned size)
{
    QStreamState *s = opaque;

    if (size != 4)
        return ~0ULL;

    switch (addr) {
    case QSTREAM_REG_MAGIC:
        return QSTREAM_MAGIC;

    case QSTREAM_REG_VERSION:
        return QSTREAM_VERSION;

    case QSTREAM_REG_SCRATCH:
        return s->scratch;

    case QSTREAM_REG_IRQ_STATUS:
        return s->irq_status;

    default:
        return ~0ULL;
    }
}

static void qstream_mmio_write(void *opaque,
                               hwaddr addr,
                               uint64_t value,
                               unsigned size)
{
    QStreamState *s = opaque;

    if (size != 4)
        return;

    switch (addr) {
    case QSTREAM_REG_SCRATCH:
        s->scratch = (uint32_t)value;
        break;

    case QSTREAM_REG_IRQ_RAISE:
        qstream_raise_irq(s, (uint32_t)value);
        break;

    case QSTREAM_REG_IRQ_ACK:
        qstream_ack_irq(s, (uint32_t)value);
        break;

    default:
        break;
    }
}

static const MemoryRegionOps qstream_mmio_ops = {
    .read = qstream_mmio_read,
    .write = qstream_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,

    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },

    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void qstream_realize(PCIDevice *pdev, Error **errp)
{
    QStreamState *s = QSTREAM_DEVICE(pdev);

    s->scratch = 0;
    s->irq_status = 0;

    memory_region_init_io(&s->bar0,
                          OBJECT(s),
                          &qstream_mmio_ops,
                          s,
                          "qstream-bar0",
                          QSTREAM_BAR_SIZE);
    pci_config_set_interrupt_pin(pdev->config, 1);
    
    pci_register_bar(pdev,
                     QSTREAM_BAR_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar0);
}

static void qstream_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci_class = PCI_DEVICE_CLASS(klass);

    pci_class->realize = qstream_realize;
    pci_class->vendor_id = QSTREAM_PCI_VENDOR_ID;
    pci_class->device_id = QSTREAM_PCI_DEVICE_ID;
    pci_class->revision = 0x01;
    pci_class->class_id = PCI_CLASS_OTHERS;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void qstream_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    };

    static const TypeInfo qstream_info = {
        .name = TYPE_QSTREAM_DEVICE,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(QStreamState),
        .class_init = qstream_class_init,
        .interfaces = interfaces,
    };

    type_register_static(&qstream_info);
}

type_init(qstream_register_types)