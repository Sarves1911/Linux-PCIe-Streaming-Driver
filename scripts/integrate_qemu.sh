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

QEMU_SOURCE="${1:-$REPO_ROOT/.deps/qemu}"
MISC_DIR="$QEMU_SOURCE/hw/misc"
KCONFIG_FILE="$MISC_DIR/Kconfig"
MESON_FILE="$MISC_DIR/meson.build"

if [[ ! -f "$KCONFIG_FILE" || ! -f "$MESON_FILE" ]]; then
    echo "QEMU source tree not found at: $QEMU_SOURCE"
    exit 1
fi

DEVICE_SOURCE="$REPO_ROOT/device/qemu/qstream.c"
WIRE_HEADER="$REPO_ROOT/protocol/qstream_wire.h"

DEVICE_LINK="$(
    realpath --relative-to="$MISC_DIR" "$DEVICE_SOURCE"
)"

HEADER_LINK="$(
    realpath --relative-to="$MISC_DIR" "$WIRE_HEADER"
)"

ln -sfn "$DEVICE_LINK" "$MISC_DIR/qstream.c"
ln -sfn "$HEADER_LINK" "$MISC_DIR/qstream_wire.h"

if ! grep -q '^config QSTREAM$' "$KCONFIG_FILE"; then
    cat >> "$KCONFIG_FILE" <<'EOF'

config QSTREAM
    bool
    default y
    depends on PCI
EOF

    echo "Added CONFIG_QSTREAM to hw/misc/Kconfig"
else
    echo "CONFIG_QSTREAM already exists"
fi

MESON_ENTRY="system_ss.add(when: 'CONFIG_QSTREAM', if_true: files('qstream.c'))"

if ! grep -Fq "$MESON_ENTRY" "$MESON_FILE"; then
    printf '\n%s\n' "$MESON_ENTRY" >> "$MESON_FILE"
    echo "Added qstream.c to hw/misc/meson.build"
else
    echo "qstream.c Meson entry already exists"
fi

echo "QEMU integration complete"
echo "Source tree: $QEMU_SOURCE"