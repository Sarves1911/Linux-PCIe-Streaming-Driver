# qstream — Emulated PCI Streaming Driver

`qstream` is an educational hardware/software co-design project that
implements a complete streaming path from an emulated PCI device to a
multithreaded C++17 application.

The project includes:

- A custom QEMU PCI device
- A Linux kernel PCI character driver
- A memory-mapped bounded ring buffer
- A reusable C++17 runtime
- Concurrent-consumer, reset, overflow, interrupt-batching, and profiling tests

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

The driver does not busy-wait. Applications sleep through `poll()` and a
kernel wait queue until the interrupt handler places records in the shared
ring.

## Components

| Path | Purpose |
| --- | --- |
| `device/qemu/qstream.c` | QEMU PCI device model |
| `protocol/qstream_wire.h` | Shared hardware-register and record definitions |
| `driver/qstream_pci.c` | Linux PCI character driver |
| `include/uapi/` | Kernel/userspace ABI definitions |
| `runtime/` | C++17 runtime and command-line application |
| `tests/` | Correctness and stress tests |
| `scripts/` | QEMU integration, launch, testing, and profiling scripts |
| `profiling/results/` | Representative profiling results |
| `docs/` | Architecture and profiling documentation |

## QEMU Device

The custom device uses PCI ID `1234:11e9` and exposes a 4 KiB BAR0 register
window.

It implements:

- A 64-record hardware FIFO
- Fixed 32-byte records
- Eight records generated every 250 ms
- Legacy level-triggered INTx interrupts
- Start, stop, reset, and single-record generation controls
- Generation, produced-record, and hardware-drop counters
- Deterministic XOR checksums

Every timer tick generates a batch of eight records. The data-ready interrupt
remains asserted while records remain in the hardware FIFO. The driver drains
the records and acknowledges the interrupt.

### Record format

| Field | Size |
| --- | ---: |
| Sequence number | 64 bits |
| Timestamp | 64 bits |
| Generated value | 32 bits |
| Reset generation | 32 bits |
| Flags | 32 bits |
| Checksum | 32 bits |

The sequence, generated value, generation, and checksum make the stream
deterministic and testable.

## BAR0 Register Interface

BAR0 is a 4 KiB address window. The implemented registers occupy its first
few bytes:

| Offset | Register | Purpose |
| ---: | --- | --- |
| `0x00` | `MAGIC` | Identifies the qstream device |
| `0x04` | `VERSION` | Hardware-interface version |
| `0x08` | `SCRATCH` | Read/write MMIO communication test |
| `0x0C` | `IRQ_STATUS` | Reports pending interrupt reasons |
| `0x10` | `IRQ_RAISE` | Test register for raising interrupts |
| `0x14` | `IRQ_ACK` | Acknowledges pending interrupts |
| `0x18` | `CONTROL` | Generate, start, stop, and reset commands |
| `0x1C` | `FIFO_COUNT` | Number of hardware records waiting |
| `0x20–0x3C` | `DATA` | Eight 32-bit words of the front record |
| `0x40` | `FIFO_POP` | Removes the front hardware record |
| `0x44` | `STATUS` | Reports whether streaming is active |
| `0x48` | `GENERATION` | Current reset generation |
| `0x4C–0x50` | `GENERATED` | 64-bit generated-record counter |
| `0x54–0x58` | `HW_DROPS` | 64-bit hardware-drop counter |

The BAR is not ordinary RAM. Each MMIO read or write invokes logic in the
QEMU device model.

## Linux Driver

The kernel module:

- Matches PCI device `1234:11e9`
- Enables the PCI device and reserves BAR0
- Maps BAR0 with `pci_iomap()`
- Handles shared legacy interrupts
- Drains multiple FIFO records per interrupt
- Validates record checksums and reset generations
- Stores records in a bounded 1024-record kernel ring
- Exposes `/dev/qstream0`
- Supports `poll()`, `mmap()`, and `ioctl()`
- Maps the ring read-only to userspace
- Uses a wait queue instead of busy-waiting
- Tracks interrupts, drops, corruption, and consumption
- Enforces one direct mapped-ring owner at a time

Supported ioctls are:

| Ioctl | Purpose |
| --- | --- |
| `START` | Begin timer-driven record generation |
| `STOP` | Stop record generation |
| `RESET` | Clear device and driver state and advance generation |
| `CONSUME` | Advance the validated ring tail |
| `GET_STATS` | Return hardware and driver statistics |

## Shared Ring Buffer

The mapped region contains:

```text
4 KiB metadata header
1024 × 32-byte records
```

The ring uses monotonically increasing `head` and `tail` counters:

```text
available = head - tail
index = counter & (capacity - 1)
```

The ring is empty when:

```text
head == tail
```

The ring is full when:

```text
head - tail == capacity
```

The interrupt handler is the ring producer. The runtime drain thread is the
direct ring consumer.

The driver never overwrites unread records. If the ring is full, it drops the
new record and increments the software-drop counter.

## C++17 Runtime

`QStreamRuntime` provides:

- Device ownership
- Read-only ring mapping
- Start, stop, and reset control
- Blocking `poll()` notification
- Record access by ring offset
- Batched consumption
- Statistics queries

`QStreamThreadedRuntime` adds:

- One background ring-drain thread
- A bounded userspace work queue
- Condition-variable blocking
- Multiple concurrent worker threads
- Clean shutdown
- Background exception propagation

Only the drain thread advances the kernel ring tail. Concurrent worker
threads consume copied 32-byte records from the protected userspace queue.
This avoids races between multiple direct consumers of the mapped ring.

## Host and Guest Layout

The development setup is:

```text
Windows 11
└── WSL2 Ubuntu host
    └── QEMU virtual machine
        ├── qstream PCI device
        └── Ubuntu guest
            ├── qstream_pci.ko
            └── C++17 runtime and tests
```

QEMU runs on the WSL host. The Linux driver and C++ runtime run inside the
QEMU guest, because that guest is where the emulated PCI device is visible.

## Building QEMU

The QEMU source tree is intentionally excluded from Git.

The project was developed with QEMU 8.2.2. Place its source tree at:

```text
.deps/qemu
```

Integrate the qstream source and build-system entries:

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

Confirm that QEMU recognizes the custom device:

```bash
./qemu-system-x86_64 -device help | grep qstream
```

Expected output includes:

```text
name "qstream", bus PCI
```

## Running the Guest

The launcher expects these guest files:

```text
.deps/vm/qstream-guest.qcow2
.deps/vm/seed.img
```

These large generated files are intentionally excluded from Git.

Start the guest from the WSL host:

```bash
cd ~/src/qstream
./scripts/run_guest.sh
```

Inside the guest, mount the shared repository:

```bash
sudo mkdir -p /mnt/qstream

sudo mount -t 9p \
  -o trans=virtio,version=9p2000.L \
  qstream /mnt/qstream
```

Confirm that the custom PCI device is visible:

```bash
lspci -nn -d 1234:11e9
```

Expected output includes:

```text
00:03.0 Unclassified device: Device [1234:11e9]
```

## Complete Test Suite

Inside the guest, run:

```bash
sudo bash /mnt/qstream/scripts/test_guest.sh
```

The script:

1. Builds the kernel module.
2. Configures and builds the C++17 programs.
3. Loads `qstream_pci.ko`.
4. Runs every correctness and stress test.
5. Unloads the module afterward.

The suite covers:

| Test | Coverage |
| --- | --- |
| `qstream_runtime_test` | mmap, poll, record delivery, and consumption |
| `qstream_stats_test` | End-to-end hardware and driver counters |
| `qstream_reset_test` | Generation changes and state clearing |
| `qstream_concurrent_test` | Four concurrent userspace consumers |
| `qstream_burst_test` | Sustained batched interrupt delivery |
| `qstream_overflow_test` | Bounded-ring overflow and drop accounting |

The overflow test intentionally waits approximately 40 seconds.

## Profiling

The project includes repeatable profiling using `perf`, `strace`, and
`ftrace`.

Build and load the driver inside the guest:

```bash
cd /mnt/qstream/driver

make -C /lib/modules/$(uname -r)/build \
  M=$PWD \
  modules

sudo insmod ./qstream_pci.ko
```

Build the userspace programs if necessary:

```bash
cmake -S /mnt/qstream \
  -B /tmp/qstream-build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /tmp/qstream-build -j2
```

Run all three profiling tools:

```bash
sudo bash /mnt/qstream/scripts/profile_qstream.sh \
  /tmp/qstream-build \
  /mnt/qstream/profiling/results
```

Unload the driver afterward:

```bash
sudo rmmod qstream_pci
```

Representative profiling processed 256 records using four consumers with:

- Zero checksum errors
- Zero generation errors
- Zero sequence errors
- Zero dropped records
- 32 interrupts, or eight records per interrupt
- Approximately 137.63 ms of task CPU time over 8.16 seconds

See [`docs/profiling.md`](docs/profiling.md) for the complete methodology,
results, and limitations.

## Current Limitations

- The current device is conventional PCI using legacy INTx, not PCIe/MSI.
- Device records are transferred through MMIO rather than DMA.
- The kernel ring has one producer and one direct userspace drain thread.
- Concurrent workers consume from a separate bounded userspace queue.
- The configured emulated stream rate is not a maximum-throughput result.
- QEMU runs under TCG inside WSL2, so measurements demonstrate software
  behavior rather than physical PCI performance.
- Device hot-unplug and multiple independent processes are not supported.

## Project Status

The Version 1 MMIO streaming path is implemented and tested:

- Custom QEMU device: complete
- Linux PCI driver: complete
- Shared mmap ring: complete
- C++17 runtime: complete
- Multithreaded dispatch: complete
- Correctness and stress tests: complete
- `perf`, `strace`, and `ftrace` profiling: complete

A possible future Version 2 could replace MMIO FIFO draining with emulated DMA,
MSI-X, and multiple hardware queues.