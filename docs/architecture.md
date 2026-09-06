# qstream Architecture

## 1. Goal

qstream models a PCI-connected streaming device such as an FPGA,
software-defined radio, camera, or data-acquisition card.

The device continuously produces fixed-size records. A Linux kernel
driver receives those records and places them into a bounded ring
buffer. A C++17 runtime controls the device and consumes records from
userspace.

The central problem is moving an asynchronous hardware stream into an
application without busy-waiting, unbounded memory use, silent
overwrites, or unnecessary kernel-to-userspace copies.

## 2. System Layout

The system contains three main components:

1. A custom QEMU PCI device that acts as the hardware.
2. A Linux PCI character driver that manages the device.
3. A C++17 runtime used by applications.

The QEMU process runs on the development host. The driver and runtime
run inside a Linux guest created by QEMU.

The driver exposes the device as:

    /dev/qstream0

## 3. Version 1 Data Path

Version 1 uses an MMIO-accessed hardware FIFO and a driver-owned
software ring.

The data path is:

    QEMU generator
        -> hardware FIFO
        -> PCI interrupt
        -> Linux driver
        -> software ring buffer
        -> mmap
        -> C++ runtime

The device creates records and places them in its internal FIFO. When
the FIFO reaches its configured watermark, the device raises an
interrupt.

The driver handles the interrupt, drains records from the FIFO through
MMIO, and inserts them into its software ring. It then wakes any
application waiting for data.

The runtime maps the complete software ring once during initialization.
Individual records are not separately mapped or passed to userspace.

## 4. Control and Notification Paths

The control path is:

    Runtime -> ioctl -> driver -> BAR0 MMIO registers -> device

The runtime uses ioctls to configure, start, stop, reset, and query the
device.

The notification path is:

    Device interrupt -> driver wait queue -> runtime poll()

Interrupts are notifications, not containers for data. After waking,
the runtime always checks the ring state because interrupts may be
combined or may arrive before the runtime begins waiting.

## 5. Record Format

Each generated record is 32 bytes:

    sequence       64 bits
    timestamp_ns   64 bits
    value           32 bits
    generation      32 bits
    flags           32 bits
    checksum        32 bits

`sequence` increases for every record generated within one device
generation.

`timestamp_ns` is the QEMU virtual-clock timestamp at which the record
was generated.

`value` contains deterministic synthetic data derived from the sequence
number. This allows tests to reproduce and verify the stream.

`generation` changes after every device reset. It distinguishes valid
records from records created before a reset.

`flags` describes conditions such as burst generation or injected test
faults.

`checksum` allows the driver and runtime to detect damaged records.

## 6. Ring-Buffer Model

The software ring has a fixed power-of-two capacity.

It uses monotonically increasing 64-bit counters:

- `head` is the sequence position where the driver will write next.
- `tail` is the oldest position not yet consumed by the runtime.

The array position is calculated as:

    index = counter & (capacity - 1)

The ring is empty when:

    head == tail

The ring is full when:

    head - tail == capacity

The driver owns and advances `head`.

The runtime requests consumption through an ioctl, but the driver
validates and advances `tail`.

Unread records are never overwritten.

## 7. Overflow Policy

There are two independently measured overflow locations.

### Hardware FIFO overflow

If the QEMU FIFO is full, the device drops the newly generated record
and increments `hardware_drops`.

### Software ring overflow

If the driver ring is full, the driver drops the newly received record
and increments `software_drops`.

In both cases, existing unread records remain unchanged.

Sequence numbers and drop counters allow the tests to explain every
missing record.

## 8. Runtime Responsibilities

The C++ runtime is not only a display program. It provides a reusable
userspace API that:

- Opens and closes `/dev/qstream0`.
- Maps and unmaps the ring.
- Configures and controls the stream.
- Waits for records using `poll()`.
- Returns contiguous batches of available records.
- Validates sequences, generations, and checksums.
- Marks processed batches as consumed.
- Reports device and driver statistics.
- Optionally dispatches records to worker threads.

A separate `qstream-dump` application will use the runtime to print
records for demonstration.

## 9. Concurrency Model

Version 1 allows one process to own the device.

The mapped ring has:

- One producer: the Linux driver.
- One consumer: the runtime's ring-drain thread.

The runtime may later dispatch copied work to multiple worker threads.

True concurrent direct consumption from mapped ring slots is reserved
for a later milestone because it requires per-slot ownership and
ordered release.

## 10. Reset Semantics

A controlled reset performs the following operations:

1. Stop record generation.
2. Mask device interrupts.
3. Synchronize with any active interrupt handler.
4. Clear the hardware FIFO.
5. Clear the software-ring head and tail.
6. Increment the generation number.
7. Restore the configured state.
8. Wake blocked runtime threads.

A batch acquired before reset cannot be consumed after reset. The
driver rejects it as stale by comparing generation numbers.

## 11. Version 1 Non-Goals

Version 1 does not include:

- Ethernet or the Linux networking stack.
- A physical PCIe link or real-hardware throughput claims.
- Device hot-unplug.
- Multiple processes consuming the same ring.
- Direct DMA into the mapped ring.
- Multiple queues or MSI-X.

These can be added after the basic system is correct.

## 12. Planned Version 2

Version 2 replaces the MMIO FIFO transfer with an emulated DMA ring.

The device will write records directly into driver-allocated guest
memory. The same memory can then be mapped into userspace.

This removes the device-to-driver copying step and more closely models
a high-throughput FPGA or acquisition card.