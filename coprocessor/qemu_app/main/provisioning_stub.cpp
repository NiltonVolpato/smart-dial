// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// QEMU emulates no Bluetooth: provisioning is never active.

#include <cstdint>

#include "provisioning/improv_ble.h"

namespace coprocessor {

ESP_EVENT_DEFINE_BASE(PROVISIONING_EVENT);

void provisioning_start(uint32_t /*timeout_seconds*/) {}
void provisioning_stop() {}
bool provisioning_is_active() { return false; }

}  // namespace coprocessor
