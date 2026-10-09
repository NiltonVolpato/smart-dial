// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// QEMU emulates no Bluetooth: provisioning is never active.

#include <cstdint>

#include "provisioning/ble_improv.h"

namespace coprocessor {

ESP_EVENT_DEFINE_BASE(PROVISIONING_EVENT);

void ble_init() {}
void ble_start(uint32_t /*timeout_seconds*/) {}
void ble_stop() {}
bool ble_is_active() { return false; }

}  // namespace coprocessor
