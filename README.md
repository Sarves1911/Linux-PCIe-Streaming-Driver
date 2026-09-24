# qstream — Emulated PCI Streaming Driver

`qstream` is an educational hardware/software co-design project that
implements a complete streaming path from an emulated PCI device to a
multithreaded C++17 application.

The project includes:

- A custom QEMU PCI device
- A Linux kernel PCI character driver
- A memory-mapped bounded ring buffer
- A reusable C++17 runtime
- Concurrent-consumer, reset, overflow, interrupt-burst, and profiling tests

## Architecture

```text
QEMU qstream device
    │
    │ timer generates records
    ▼
64-record hardware FIFO
    │
    │ legacy PCI interrupt
    ▼
Linux qstream PCI driver
    │
    │ MMIO reads
    ▼
1024-record kernel ring
    │
    │ read-only mmap
    ▼
C++ ring-drain thread
    │
    │ bounded work queue
    ▼
Concurrent consumer threads
```

The driver does not busy-wait. It sleeps applications through `poll()` and a
kernel wait queue until an interrupt handler places records in the shared ring.

## Components

| Path | Purpose |
| --- | --- |
| `device/qemu/qstream.c` | QEMU PCI device model |
| `protocol/qstream_wire.h` | Shared hardware register and record definitions |
| `driver/qstream_pci.c` | Linux PCI character driver |
| `include/uapi/` | Kernel/userspace ABI definitions |
| `runtime/` | C++17 runtime and command-line application |
| `tests/` | Correctness and stress tests |
| `scripts/` | QEMU integration, launch, test, and profiling scripts |
| `profiling/results/` | Representative profiling results |
| `docs/` | Architecture and profiling documentation |

## QEMU Device

The custom device uses PCI ID `1234:11e9` and exposes a 4 KiB BAR0 register
window.

It implements:

- A 64-record hardware FIFO
- 32-byte deterministic records
- Eight records generated every 250 ms
- Legacy INTx interrupts
- Start, stop, and reset controls
- Generation, produced-record, and hardware-drop counters
- Deterministic XOR checksums

Each record contains:

| Field | Size |
| --- | ---: |
| Sequence number | 64 bits |
| Timestamp | 64 bits |
| Generated value | 32 bits |
| Reset generation | 32 bits |
| Flags | 32 bits |
| Checksum | 32 bits |

## Linux Driver

The kernel module:

- Matches PCI device `1234:11e9`
- Enables and reserves BAR0
- Maps registers with `pci_iomap()`
- Handles shared legacy interrupts
- Drains multiple FIFO records per interrupt
- Validates checksums and reset generations
- Stores records in a bounded 1024-record ring
- Exposes `/dev/qstream0`
- Supports `poll()`, `mmap()`, and `ioctl()`
- Maps the ring read-only to userspace
- Uses a wait queue instead of busy-waiting
- Tracks interrupt, drop, corruption, and consumption statistics
- Enforces one direct ring owner at a time

Supported ioctls include:

- `START`
- `STOP`
- `RESET`
- `CONSUME`
- `GET_STATS`

## C++17 Runtime

`QStreamRuntime` provides direct control of the mapped kernel ring.

`QStreamThreadedRuntime` adds:

- One background ring-drain thread
- A bounded userspace queue
- Condition-variable blocking
- Multiple concurrent worker threads
- Clean stop and exception propagation

Only the drain thread advances the kernel ring tail. Worker threads consume
from the protected userspace queue, avoiding races between multiple direct
ring consumers.

## Building QEMU

The QEMU source tree is intentionally excluded from Git.

The project was tested with QEMU 8.2.2. Place its source at:

```text
.deps/qemu
```

Integrate the qstream device into that source tree:

```bash
./scripts/integrate_qemu.sh
```

Configure and build QEMU:

```bash
mkdir -p .deps/qemu/build
cd .deps/qemu/build

../configure --target-list=x86_64-softmmu
ninja -j4
```

Confirm that QEMU recognizes the device:

```bash
./qemu-system-x86_64 -device help | grep qstream
```

Expected output includes:

```text
name "qstream", bus PCI
```

## Running the Guest

The launcher expects these prepared guest files:

```text
.deps/vm/qstream-guest.qcow2
.deps/vm/seed.img
```

Start the guest from the WSL host:

```bash
./scripts/run_guest.sh
```

Inside the guest, mount the shared repository:

```bash
sudo mkdir -p /mnt/qstream

sudo mount -t 9p \
  -o trans=virtio,version=9p2000.L \
  qstream /mnt/qstream
```

Confirm that the device is visible:

```bash
lspci -nn -d 1234:11e9
```

## Complete Test Suite

Inside the guest, run:

```bash
sudo bash /mnt/qstream/scripts/test_guest.sh
```

This builds the driver and C++ programs, loads the module, runs every test, and
unloads the module afterward.

The suite covers:

- Mapped-ring record delivery
- End-to-end statistics
- Controlled device resets
- Four concurrent userspace consumers
- Sustained interrupt bursts
- Bounded ring overflow
- Checksums and sequence continuity

The overflow test intentionally takes approximately 40 seconds.

## Profiling

The project includes repeatable profiling with `perf`, `strace`, and `ftrace`.

Inside the guest:

```bash
sudo bash /mnt/qstream/scripts/profile_qstream.sh \
  /tmp/qstream-build \
  /mnt/qstream/profiling/results
```

Representative measurements processed 256 records using four consumers with:

- Zero checksum errors
- Zero sequence errors
- Zero dropped records
- 32 interrupts, or eight records per interrupt
- Approximately 137.63 ms of task CPU time over 8.16 seconds

See [`docs/profiling.md`](docs/profiling.md) for methodology and limitations.

## Current Limitations

- The current QEMU model is conventional PCI using legacy INTx, not PCIe/MSI.
- Device records are transferred through MMIO rather than DMA.
- The kernel ring has one producer and one direct userspace drain thread.
- Concurrent workers consume from a separate bounded userspace queue.
- QEMU runs under TCG inside WSL2, so measurements demonstrate software
  behavior rather than physical PCI hardware performance.

## License

This project is currently intended for educational and portfolio use.