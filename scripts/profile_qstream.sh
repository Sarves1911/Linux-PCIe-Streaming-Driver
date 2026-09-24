#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${1:-/tmp/qstream-build}"
RESULT_DIR="${2:-/tmp/qstream-profile-$(date +%Y%m%d-%H%M%S)}"
TRACE_DIR="/sys/kernel/tracing"

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo"
    exit 1
fi

if [[ ! -e /dev/qstream0 ]]; then
    echo "/dev/qstream0 does not exist; load qstream_pci.ko first"
    exit 1
fi

for executable in qstream_concurrent_test qstream_runtime_test; do
    if [[ ! -x "$BUILD_DIR/$executable" ]]; then
        echo "Missing executable: $BUILD_DIR/$executable"
        exit 1
    fi
done

mkdir -p "$RESULT_DIR"

{
    date --iso-8601=seconds
    uname -a
    echo "Build directory: $BUILD_DIR"
} > "$RESULT_DIR/environment.txt"

echo "Running perf..."
perf stat \
    -o "$RESULT_DIR/perf-stat.txt" \
    -e task-clock,context-switches,cpu-migrations,page-faults \
    "$BUILD_DIR/qstream_concurrent_test" \
    > "$RESULT_DIR/perf-workload.txt" 2>&1

echo "Running strace..."
strace -f -c \
    -o "$RESULT_DIR/strace-summary.txt" \
    "$BUILD_DIR/qstream_concurrent_test" \
    > "$RESULT_DIR/strace-workload.txt" 2>&1

cleanup_ftrace()
{
    echo 0 > "$TRACE_DIR/tracing_on" || true
    echo nop > "$TRACE_DIR/current_tracer" || true
    : > "$TRACE_DIR/set_ftrace_filter" || true
}

trap cleanup_ftrace EXIT

echo "Running ftrace..."
cleanup_ftrace
: > "$TRACE_DIR/trace"

printf '%s\n' \
    qstream_irq_handler \
    qstream_ioctl \
    qstream_poll \
    qstream_mmap \
    > "$TRACE_DIR/set_ftrace_filter"

echo function_graph > "$TRACE_DIR/current_tracer"
echo 1 > "$TRACE_DIR/tracing_on"

"$BUILD_DIR/qstream_runtime_test" \
    > "$RESULT_DIR/ftrace-workload.txt" 2>&1

echo 0 > "$TRACE_DIR/tracing_on"
cp "$TRACE_DIR/trace" "$RESULT_DIR/ftrace.txt"

cleanup_ftrace
trap - EXIT

if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
    chown -R "$SUDO_UID:$SUDO_GID" "$RESULT_DIR"
fi

echo "Profiling complete"
echo "Results: $RESULT_DIR"