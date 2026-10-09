#!/usr/bin/env bash
# Copyright © 2026 Nilton Volpato
# SPDX-License-Identifier: MIT
#
# Automated QEMU test runner for the coprocessor:
# 1. Ensures QEMU app is built.
# 2. Starts QEMU in the background with OpenEth and UART1 pty.
# 3. Runs the Rust qemu-driver integration suite against the pty.
# 4. Cleans up QEMU on exit.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build="$root/.cache/coprocessor/qemu"
uart="$build/uart1"
qemu_log="$build/qemu.log"

# Always source ESP-IDF v6.1 export.sh to set up virtualenv and python packages
if [ -f "/Users/nilton/.espressif/v6.1/esp-idf/export.sh" ]; then
  . /Users/nilton/.espressif/v6.1/esp-idf/export.sh >/dev/null 2>&1
fi

# Detect host Mac IP for GENA callbacks from physical speakers
HOST_IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)
if [ -z "$HOST_IP" ]; then
  HOST_IP="127.0.0.1"
fi
SONOS_CALLBACK_PORT=18088
QEMU_FWD_PORT=18080

mkdir -p "$build"
cat << EOF > "$build/sonos_callback_config.h"
#pragma once
#define SONOS_CALLBACK_HOST "$HOST_IP"
#define SONOS_CALLBACK_PORT $SONOS_CALLBACK_PORT
EOF

# 1. Build firmware
echo "=== Building QEMU coprocessor firmware (Callback: $HOST_IP:$SONOS_CALLBACK_PORT) ==="
idf.py -C "$here" -B "$build" build

rm -f "$uart" "$build/qemu_flash.bin"

# 2. Start Nginx reverse proxy for GENA NOTIFY
nginx_pid=""
if command -v nginx >/dev/null 2>&1; then
  nginx_conf="$build/nginx.conf"
  cat << EOF > "$nginx_conf"
daemon off;
pid $build/nginx.pid;
error_log $build/nginx_error.log;
events { worker_connections 64; }
http {
  access_log $build/nginx_access.log;
  server {
    listen $SONOS_CALLBACK_PORT;
    location /media/notify {
      proxy_pass http://127.0.0.1:$QEMU_FWD_PORT;
    }
  }
}
EOF
  echo "=== Starting Nginx reverse proxy on $HOST_IP:$SONOS_CALLBACK_PORT -> 127.0.0.1:$QEMU_FWD_PORT ==="
  nginx -c "$nginx_conf" > "$build/nginx.log" 2>&1 &
  nginx_pid=$!
else
  echo "Warning: nginx not found in PATH, GENA callbacks from physical Sonos may not reach QEMU"
fi

echo "=== Launching QEMU in background ==="
# Launch QEMU with its own process group
set -m
idf.py -C "$here" -B "$build" qemu --qemu-extra-args="-nic user,model=open_eth,hostfwd=tcp:127.0.0.1:$QEMU_FWD_PORT-:80 -serial pty:$uart" \
  > "$qemu_log" 2>&1 < /dev/null &
qemu_pid=$!

cleanup() {
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "=== QEMU Log Tail on Failure ==="
    tail -n 30 "$qemu_log" || true
    if [ -f "$build/nginx_error.log" ]; then
      echo "=== Nginx Error Log Tail ==="
      tail -n 30 "$build/nginx_error.log" || true
    fi
  fi
  if [ -n "${qemu_pid:-}" ]; then
    echo "=== Stopping QEMU (PID $qemu_pid) ==="
    kill -- -"$qemu_pid" 2>/dev/null || kill "$qemu_pid" 2>/dev/null || true
  fi
  if [ -n "${nginx_pid:-}" ]; then
    echo "=== Stopping Nginx (PID $nginx_pid) ==="
    kill "$nginx_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=== Waiting for UART1 PTY to appear at $uart ==="
deadline=$((SECONDS + 30))
while [ ! -e "$uart" ]; do
  if [ $SECONDS -ge $deadline ]; then
    echo "Error: Timed out waiting for UART1 PTY $uart"
    echo "QEMU log:"
    cat "$qemu_log"
    exit 1
  fi
  sleep 0.2
done

echo "=== Running Rust QEMU driver test suite ==="
TETHER_UART="$uart" TETHER_UART_BAUD=0 \
  cargo run --manifest-path "$root/portable/Cargo.toml" -p qemu-driver "$@"

echo "=== QEMU integration test successfully finished ==="
