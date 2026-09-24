# qstream Profiling Results

## Purpose

This document records profiling of the complete qstream data path:

```text
QEMU device
    -> PCI interrupt
    -> Linux interrupt handler
    -> kernel ring buffer
    -> mmap
    -> C++ drain thread
    -> concurrent consumer threads
```

The project was profiled using:

- `perf` for process CPU and scheduling activity
- `strace` for userspace system-call behavior
- `ftrace` for Linux driver function execution

## Environment

The measurements were collected inside an Ubuntu 24.04 QEMU guest using:

```text
Kernel: 6.8.0-139-generic
Architecture: x86-64
QEMU acceleration: TCG
Guest CPUs: 2
```

KVM is unavailable inside WSL2, so QEMU uses TCG software emulation.

These measurements validate the software behavior and relative overhead of
the design. They must not be interpreted as physical PCI hardware latency,
maximum throughput, or native CPU performance measurements.

The emulated device was configured to generate:

```text
8 records every 250 milliseconds
32 records per second
```

The measured record rate is therefore the configured generation rate, not
the maximum throughput of the driver or runtime.

## Workload

The main profiling workload used `qstream_concurrent_test` with:

- One background ring-drain thread
- Four concurrent consumer threads
- A bounded userspace work queue
- 256 total records
- Eight records delivered per device interrupt

The test completed with:

```text
processed=256
checksum_errors=0
generation_errors=0
sequence_errors=0
interrupts=32
kernel_pending=0
userspace_queued=0
```

No records were corrupted, duplicated, lost, or left pending.

## perf Results

The following command measured CPU and scheduler activity:

```bash
sudo perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  /tmp/qstream-build/qstream_concurrent_test
```

Representative results:

| Measurement | Result |
| --- | ---: |
| Elapsed time | 8.162 seconds |
| Task CPU time | 137.63 ms |
| CPU utilization | 0.017 CPUs |
| Context switches | 278 |
| CPU migrations | 25 |
| Page faults | 151 |

The process used approximately 1.7% of one guest CPU during the test.

This is consistent with the intended blocking design: threads spend most of
their time asleep while waiting for records instead of continuously polling
the CPU.

These values include TCG virtualization overhead and should not be compared
directly with native hardware results.

## strace Results

The following command summarized system calls from the main thread, drain
thread, and consumer threads:

```bash
sudo strace -f -c \
  /tmp/qstream-build/qstream_concurrent_test
```

Important results:

| System call | Calls | Purpose |
| --- | ---: | --- |
| `futex` | 1673 | Condition-variable sleeping and waking |
| `clock_gettime` | 511 | Timeout and timing operations |
| `poll` | 97 | Blocking until driver data becomes available |
| `ioctl` | 37 | Device control, consumption, and statistics |
| `clone3` | 5 | One drain thread and four consumer threads |
| `mmap` | 33 | Program/library mappings plus the qstream ring |

The large percentage assigned to `futex` represents accumulated waiting time
across several threads. It does not mean the process continuously consumed
the CPU.

The runtime processed 256 records with only 37 total `ioctl` calls because
records were handled in batches rather than using one control call per
record.

The qstream data path does not transfer individual records through `read()`.
The kernel ring is mapped once, and userspace accesses records through that
mapping. The `read` calls shown by `strace` belong to normal program and
dynamic-loader activity.

## ftrace Results

The Linux function-graph tracer monitored:

- `qstream_mmap`
- `qstream_ioctl`
- `qstream_poll`
- `qstream_irq_handler`

A representative trace was:

```text
CPU 0: qstream_mmap()
CPU 0: qstream_ioctl()
CPU 0: qstream_poll()
CPU 1: qstream_irq_handler()
CPU 0: qstream_poll()
CPU 0: qstream_ioctl()
```

The observed event order confirms the intended data path:

1. The runtime maps the shared kernel ring.
2. The runtime controls the device through an ioctl.
3. The runtime enters `poll()` and sleeps.
4. QEMU generates records and raises a PCI interrupt.
5. Linux executes `qstream_irq_handler()`.
6. The handler places records into the kernel ring and wakes the wait queue.
7. `poll()` returns and the runtime processes the mapped records.
8. The runtime reports batch consumption through an ioctl.

The interrupt handler ran on a different virtual CPU from the userspace
control operations, demonstrating that record arrival is asynchronous.

Representative function-graph output:

```text
 0) ! 968.390 us  |  qstream_mmap [qstream_pci]();
 0) ! 138.861 us  |  qstream_ioctl [qstream_pci]();
 0) +  35.149 us  |  qstream_ioctl [qstream_pci]();
 0) +  25.811 us  |  qstream_poll [qstream_pci]();
 1) ! 284.154 us  |  qstream_irq_handler [qstream_pci]();
 0) ! 168.649 us  |  qstream_poll [qstream_pci]();
```

These durations include TCG emulation and tracing overhead. They are evidence
that the functions executed, not measurements of physical PCI latency.

## Reproduction

First build the userspace programs inside the guest:

```bash
cmake -S /mnt/qstream \
  -B /tmp/qstream-build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /tmp/qstream-build -j2
```

Build and load the kernel module:

```bash
cd /mnt/qstream/driver

make -C /lib/modules/$(uname -r)/build \
  M=$PWD \
  modules

sudo insmod ./qstream_pci.ko
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

The script produces:

```text
profiling/results/environment.txt
profiling/results/perf-stat.txt
profiling/results/perf-workload.txt
profiling/results/strace-summary.txt
profiling/results/strace-workload.txt
profiling/results/ftrace.txt
profiling/results/ftrace-workload.txt
```

## Conclusion

The measurements show that:

- The runtime blocks instead of busy-waiting.
- The kernel/userspace ring is mapped once.
- Records are processed in batches.
- Multiple userspace workers consume records concurrently.
- Interrupt delivery, wait-queue notification, mapped-ring access, and ioctl
  control all execute as designed.
- The measured run completed without corruption, sequence errors, or drops.

The results establish functional and architectural behavior under emulation.
Native Linux with KVM or physical hardware would be required for defensible
hardware-performance claims.