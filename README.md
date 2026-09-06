# Linux PCIe Streaming Driver & Runtime

> Work in progress. The functionality described below is the planned architecture
> and will be implemented incrementally.

`qstream` is an educational hardware/software co-design project that models a
PCI-connected streaming device and builds the Linux software stack required to
use it.

The project contains:

- A custom QEMU PCI device that generates deterministic data records.
- A Linux character driver using BAR MMIO and interrupt-driven I/O.
- An mmap-backed bounded ring buffer shared with userspace.
- A C++17 runtime for device control and batched record consumption.
- Correctness, overrun, concurrency, reset, and profiling tests.

## Version 1 Data Path

```text
QEMU generator
    -> hardware FIFO
    -> PCI interrupt
    -> Linux driver
    -> software ring buffer
    -> mmap
    -> C++17 runtime