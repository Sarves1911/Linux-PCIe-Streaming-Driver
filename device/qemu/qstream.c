#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"
#include "qstream_wire.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#define QSTREAM_STREAM_PERIOD_MS 250
#define TYPE_QSTREAM_DEVICE "qstream"
#define QSTREAM_RECORDS_PER_TICK 8u

OBJECT_DECLARE_SIMPLE_TYPE(QStreamState, QSTREAM_DEVICE)

#define QSTREAM_FIFO_CAPACITY 64u
#define QSTREAM_FIFO_MASK     (QSTREAM_FIFO_CAPACITY - 1u)

typedef struct QStreamRecord {
    uint32_t words[QSTREAM_RECORD_WORDS];
} QStreamRecord;

struct QStreamState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    uint32_t scratch;
    uint32_t irq_status;
    QStreamRecord fifo[QSTREAM_FIFO_CAPACITY];
    uint64_t fifo_head;
    uint64_t fifo_tail;
    uint64_t next_sequence;
    uint32_t generation;
    QEMUTimer *stream_timer;
    bool running;
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
    if (s->fifo_head != s->fifo_tail)
        s->irq_status |= QSTREAM_IRQ_DATA_READY;

    qstream_update_irq(s);
}

static uint32_t qstream_fifo_count(const QStreamState *s)
{
    return (uint32_t)(s->fifo_head - s->fifo_tail);
}

static bool qstream_fifo_empty(const QStreamState *s)
{
    return s->fifo_head == s->fifo_tail;
}

static bool qstream_fifo_full(const QStreamState *s)
{
    return qstream_fifo_count(s) >= QSTREAM_FIFO_CAPACITY;
}

static QStreamRecord *qstream_fifo_front(QStreamState *s)
{
    uint64_t index;

    if (qstream_fifo_empty(s))
        return NULL;

    index = s->fifo_tail & QSTREAM_FIFO_MASK;
    return &s->fifo[index];
}

static void qstream_fifo_pop(QStreamState *s)
{
    if (qstream_fifo_empty(s))
        return;

    s->fifo_tail++;

    if (qstream_fifo_empty(s))
        s->irq_status &= ~QSTREAM_IRQ_DATA_READY;

    qstream_update_irq(s);
}

static uint32_t qstream_record_checksum(const QStreamRecord *record)
{
    uint32_t checksum = 0;
    unsigned int word;

    for (word = 0; word < QSTREAM_WORD_CHECKSUM; word++)
        checksum ^= record->words[word];

    return checksum;
}

static void qstream_generate_one(QStreamState *s)
{
    QStreamRecord *record;
    uint64_t sequence;
    uint64_t timestamp;
    uint64_t index;

    if (qstream_fifo_full(s))
        return;

    index = s->fifo_head & QSTREAM_FIFO_MASK;
    record = &s->fifo[index];

    memset(record, 0, sizeof(*record));

    sequence = s->next_sequence++;
    timestamp = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    record->words[QSTREAM_WORD_SEQUENCE_LO] =
        (uint32_t)sequence;
    record->words[QSTREAM_WORD_SEQUENCE_HI] =
        (uint32_t)(sequence >> 32);

    record->words[QSTREAM_WORD_TIMESTAMP_LO] =
        (uint32_t)timestamp;
    record->words[QSTREAM_WORD_TIMESTAMP_HI] =
        (uint32_t)(timestamp >> 32);

    record->words[QSTREAM_WORD_VALUE] =
        0xA5000000u | (uint32_t)(sequence & 0xFFFFu);
    record->words[QSTREAM_WORD_GENERATION] =
        s->generation;
    record->words[QSTREAM_WORD_FLAGS] = 0;

    record->words[QSTREAM_WORD_CHECKSUM] =
        qstream_record_checksum(record);

    s->fifo_head++;

    qstream_raise_irq(s, QSTREAM_IRQ_DATA_READY);
}

static void qstream_schedule_next(QStreamState *s)
{
    int64_t deadline;

    deadline =
        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
        QSTREAM_STREAM_PERIOD_MS;

    timer_mod(s->stream_timer, deadline);
}

static void qstream_timer_callback(void *opaque)
{
    QStreamState *s = opaque;
    unsigned int i;

    if (!s->running)
        return;

    for (i = 0; i < QSTREAM_RECORDS_PER_TICK; i++)
        qstream_generate_one(s);

    if (s->running)
        qstream_schedule_next(s);
}

static void qstream_start(QStreamState *s)
{
    if (s->running)
        return;

    s->running = true;
    qstream_schedule_next(s);
}

static void qstream_stop(QStreamState *s)
{
    s->running = false;
    timer_del(s->stream_timer);
}

static void qstream_reset(QStreamState *s)
{
    qstream_stop(s);

    memset(s->fifo, 0, sizeof(s->fifo));

    s->fifo_head = 0;
    s->fifo_tail = 0;
    s->next_sequence = 0;

    s->generation++;

    s->irq_status = 0;
    qstream_update_irq(s);
}

static uint64_t qstream_mmio_read(void *opaque,
                                  hwaddr addr,
                                  unsigned size)
{
    QStreamState *s = opaque;
    QStreamRecord *record;
    unsigned int word;

    if (size != 4)
        return ~0ULL;

    if (addr >= QSTREAM_REG_DATA_BASE &&
        addr < QSTREAM_REG_DATA_BASE + QSTREAM_RECORD_SIZE) {
        record = qstream_fifo_front(s);

        if (!record)
            return ~0ULL;

        word = (unsigned int)
            ((addr - QSTREAM_REG_DATA_BASE) / 4u);

        return record->words[word];
    }

    switch (addr) {
    case QSTREAM_REG_MAGIC:
        return QSTREAM_MAGIC;

    case QSTREAM_REG_VERSION:
        return QSTREAM_VERSION;

    case QSTREAM_REG_SCRATCH:
        return s->scratch;

    case QSTREAM_REG_IRQ_STATUS:
        return s->irq_status;

    case QSTREAM_REG_FIFO_COUNT:
        return qstream_fifo_count(s);
    
    case QSTREAM_REG_STATUS:
        return s->running ? QSTREAM_STATUS_RUNNING : 0;

    case QSTREAM_REG_GENERATION:
        return s->generation;

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
    case QSTREAM_REG_CONTROL:
        if (value & QSTREAM_CONTROL_RESET) {
            qstream_reset(s);
            break;
        }

        if (value & QSTREAM_CONTROL_GENERATE_ONE)
            qstream_generate_one(s);

        if (value & QSTREAM_CONTROL_START)
            qstream_start(s);

        if (value & QSTREAM_CONTROL_STOP)
            qstream_stop(s);

        break;

    case QSTREAM_REG_FIFO_POP:
        if (value & QSTREAM_FIFO_POP_ONE)
            qstream_fifo_pop(s);
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
    memset(s->fifo, 0, sizeof(s->fifo));
    s->fifo_head = 0;
    s->fifo_tail = 0;
    s->next_sequence = 0;
    s->generation = 0;
    s->running = false;

    s->stream_timer =
    timer_new_ms(QEMU_CLOCK_VIRTUAL,
                 qstream_timer_callback,
                 s);

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

static void qstream_uninit(PCIDevice *pdev)
{
    QStreamState *s = QSTREAM_DEVICE(pdev);

    s->running = false;
    s->irq_status = 0;
    pci_set_irq(pdev, 0);

    timer_free(s->stream_timer);
    s->stream_timer = NULL;
}

static void qstream_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci_class = PCI_DEVICE_CLASS(klass);

    pci_class->realize = qstream_realize;
    pci_class->exit = qstream_uninit;
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