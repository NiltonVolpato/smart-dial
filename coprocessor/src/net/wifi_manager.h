// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "esp_err.h"
#include "esp_event.h"

namespace coprocessor {

/// Posted on the default event loop, with no data:
/// - CONNECTING: joining a network starts, or rejoining after a drop (it may
///   repeat while joining);
/// - GAVE_UP: joining failed for good: after a few retries, wifi_manager stops
///   trying (the network may be gone).
/// Joining succeeded when an IP comes (IP_EVENT_STA_GOT_IP): whether there's
/// an IP is the network module's business (net/network.h).
ESP_EVENT_DECLARE_BASE(WIFI_MANAGER_EVENT);
enum : int32_t { WIFI_MANAGER_CONNECTING, WIFI_MANAGER_GAVE_UP };

/// Starts Wi-Fi and joins the saved network, if any. Requires the default
/// event loop.
void wifi_init();
/// Joins a network, saving it for the next boot. Any task; doesn't block: the
/// event loop task does the work, posting WIFI_MANAGER_CONNECTING, then the
/// outcome comes as IP events or WIFI_MANAGER_GAVE_UP. ESP_ERR_INVALID_ARG if
/// the SSID is empty or either doesn't fit, ESP_ERR_TIMEOUT if the event loop
/// is full.
esp_err_t wifi_connect(std::string_view ssid, std::string_view password);
/// The network being joined or joined; empty if none. Any task.
std::string wifi_get_ssid();

}  // namespace coprocessor
