#ifndef QSTREAM_WIRE_H
#define QSTREAM_WIRE_H

/* Local development PCI IDs—not IDs for physical commercial hardware. */
#define QSTREAM_PCI_VENDOR_ID 0x1234
#define QSTREAM_PCI_DEVICE_ID 0x11e9

#define QSTREAM_BAR_INDEX 0
#define QSTREAM_BAR_SIZE  0x1000

#define QSTREAM_REG_MAGIC   0x00
#define QSTREAM_REG_VERSION 0x04
#define QSTREAM_REG_SCRATCH 0x08

#define QSTREAM_MAGIC   0x51535452u
#define QSTREAM_VERSION 0x00010000u

#endif