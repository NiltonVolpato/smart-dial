// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "esp_event.h"

namespace coprocessor {

/// Wi-Fi provisioning over BLE, with the Improv protocol (improv-wifi.com).
/// Nothing of it exists outside a session: it holds BLE (ble/ble.h) only
/// while one runs.

/// Posted on the default event loop as provisioning changes state: the event
/// id is a CoprocessorProto::ProvisioningState, with no data.
ESP_EVENT_DECLARE_BASE(PROVISIONING_EVENT);

/// Opens a session for `timeout_seconds` (0: 3 minutes; at most an hour), or
/// extends the open one. Credentials received go to wifi_connect(). Any task:
/// the outcome comes as PROVISIONING_EVENT (Inactive if BLE is busy).
void provisioning_start(uint32_t timeout_seconds);
/// Ends the session, if any. Any task.
void provisioning_stop();
/// Any task.
bool provisioning_is_active();

}  // namespace coprocessor
