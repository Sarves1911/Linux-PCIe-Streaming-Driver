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

QEMU_BINARY="$REPO_ROOT/.deps/qemu/build/qemu-system-x86_64"
VM_DIRECTORY="$REPO_ROOT/.deps/vm"
GUEST_DISK="$VM_DIRECTORY/qstream-guest.qcow2"
SEED_DISK="$VM_DIRECTORY/seed.img"

for required_file in \
    "$QEMU_BINARY" \
    "$GUEST_DISK" \
    "$SEED_DISK"
do
    if [[ ! -f "$required_file" ]]; then
        echo "Missing required file: $required_file"
        exit 1
    fi
done

exec "$QEMU_BINARY" \
    -name qstream-guest \
    -machine pc,accel=tcg \
    -cpu max \
    -smp 2 \
    -m 2048 \
    -drive file="$GUEST_DISK",format=qcow2,if=virtio \
    -drive file="$SEED_DISK",format=raw,if=virtio,readonly=on \
    -device qstream \
    -fsdev local,id=qstreamfs,path="$REPO_ROOT",security_model=none \
    -device virtio-9p-pci,fsdev=qstreamfs,mount_tag=qstream \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -nographic