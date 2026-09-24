# Qstream Profiling Results

## Environment

The measurements were collected inside an Ubuntu 24.04 QEMU guest using
Linux 6.8.0-139-generic. QEMU used TCG software emulation because KVM is
not available inside WSL2.

These results validate software behavior and relative overhead. They must
not be treated as physical PCI hardware latency or throughput measurements.

## Workload

The profiling workload used the multithreaded runtime with:

- One background ring-drain thread
- Four concurrent consumer threads
- 256 total records
- Eight records generated per device interrupt
- A bounded userspace work queue

The test completed with:

- 256 processed records
- Zero checksum errors
- Zero generation errors
- Zero sequence errors
- Zero hardware or software drops
- Zero pending records

## perf

The following command measured process-level resource use:

```bash
sudo perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  /tmp/qstream-build/qstream_concurrent_test
````

Representative result:

| Measurement      |        Result |
| ---------------- | ------------: |
| Elapsed time     | 8.162 seconds |
| Task CPU time    |     137.63 ms |
| CPU utilization  |    0.017 CPUs |
| Context switches |           278 |
| CPU migrations   |            25 |
| Page faults      |           151 |

The program used approximately 1.7% of one guest CPU during the test.
The low utilization is consistent with blocking waits rather than
busy-waiting.

## strace

The following command summarized userspace system calls:

```bash
sudo strace -f -c \
  /tmp/qstream-build/qstream_concurrent_test
```

Important observations:

* Five threads were created: one ring-drain thread and four consumers.
* `futex` calls came from sleeping condition-variable synchronization.
* `poll` blocked the drain thread while waiting for driver data.
* Only 37 `ioctl` calls were made while processing 256 records.
* Records were accessed through the mapped ring rather than individual
  `read()` system calls.

The high percentage attributed to `futex` represents blocked waiting
time accumulated across threads, not continuous CPU execution.

## ftrace

The kernel function-graph tracer recorded these driver functions:

* `qstream_mmap`
* `qstream_ioctl`
* `qstream_poll`
* `qstream_irq_handler`

A representative trace was:

```text
CPU 0: qstream_mmap()
CPU 0: qstream_ioctl()
CPU 0: qstream_poll()
CPU 1: qstream_irq_handler()
CPU 0: qstream_poll()
CPU 0: qstream_ioctl()
```

This confirms the complete event path:

1. Userspace maps the shared ring.
2. Userspace starts streaming through an ioctl.
3. The runtime sleeps in poll.
4. The device raises an interrupt.
5. Linux executes the driver's interrupt handler.
6. Poll wakes and the runtime consumes the mapped records.
7. Userspace reports consumption through an ioctl.

The measured function durations reflect TCG emulation overhead and are
not physical-device latency measurements.

## Reproduction

Run all three profiling tools with:

```bash
sudo bash /mnt/qstream/scripts/profile_qstream.sh \
  /tmp/qstream-build \
  /mnt/qstream/profiling/results
```
