#!/usr/bin/env bash
# Copyright © 2026 Nilton Volpato
# SPDX-License-Identifier: MIT
#
# Runs the coprocessor QEMU test app with OpenEth virtual Ethernet and UART1 on a pty.
# UART0 (console) is routed to stdio. UART1 (Tether) is routed to the pty symlink.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build="$root/.cache/coprocessor/qemu"
uart="${1:-$build/uart1}"

# Always source ESP-IDF v6.1 export.sh to set up virtualenv and python packages
if [ -f "/Users/nilton/.espressif/v6.1/esp-idf/export.sh" ]; then
  . /Users/nilton/.espressif/v6.1/esp-idf/export.sh >/dev/null 2>&1
fi

# Build if elf does not exist
if [ ! -f "$build/coprocessor_qemu.elf" ]; then
  idf.py -C "$here" -B "$build" build
fi

rm -f "$uart"
mkdir -p "$build"

echo "=== Booting QEMU with OpenEth and Tether UART1 on $uart ==="
echo "Press Ctrl+A then X to exit QEMU."

exec idf.py -C "$here" -B "$build" qemu --qemu-extra-args="-nic user,model=open_eth -serial pty:$uart"
