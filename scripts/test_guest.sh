#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
    pwd
)"

REPO_ROOT="$(
    cd -- "$SCRIPT_DIR/.."
    pwd
)"

BUILD_DIR="${1:-/tmp/qstream-build}"
DRIVER_DIR="$REPO_ROOT/driver"
MODULE="$DRIVER_DIR/qstream_pci.ko"

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo"
    exit 1
fi

cleanup()
{
    if lsmod | grep -q '^qstream_pci '; then
        rmmod qstream_pci || true
    fi
}

trap cleanup EXIT

echo "Building kernel driver..."
make -C "/lib/modules/$(uname -r)/build" \
    M="$DRIVER_DIR" \
    modules

echo "Building userspace programs..."
cmake -S "$REPO_ROOT" \
      -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" -j2

if lsmod | grep -q '^qstream_pci '; then
    echo "Reloading existing qstream driver..."
    rmmod qstream_pci
fi

insmod "$MODULE"

echo
echo "Running mapped-ring runtime test..."
"$BUILD_DIR/qstream_runtime_test"

echo
echo "Running statistics test..."
"$BUILD_DIR/qstream_stats_test"

echo
echo "Running reset test..."
"$BUILD_DIR/qstream_reset_test"

echo
echo "Running concurrent-consumer test..."
"$BUILD_DIR/qstream_concurrent_test"

echo
echo "Running interrupt-burst test..."
"$BUILD_DIR/qstream_burst_test"

echo
echo "Running bounded-overflow test..."
"$BUILD_DIR/qstream_overflow_test"

echo
echo "ALL QSTREAM TESTS PASSED"