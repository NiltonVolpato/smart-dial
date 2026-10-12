// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "net/wifi_manager.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

namespace coprocessor {

ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_EVENT);

namespace {

constexpr const char* TAG = "wifi_mgr";

// wifi_connect() requests, carrying a wifi_config_t. Private: they're how
// callers on other tasks hand the work to the event loop task.
ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_REQUEST);
enum : int32_t { kConnect };

// Failed attempts in a row, joining or rejoining after a drop: each one ends
// in STA_DISCONNECTED. Event loop task only.
//
// TODO: try ESP-IDF's roaming app (CONFIG_ESP_WIFI_ENABLE_ROAMING_APP; see
// examples/wifi/roaming/roaming_app). With several APs on one SSID, it moves
// to a stronger one: periodic scans and a low-RSSI trigger, then a reconnect
// to that BSSID (or 802.11v BTM where the APs support it). With BSSID
// blacklisting and auto-blacklisting on, a BSSID that keeps failing is
// skipped, so FAST_SCAN picking a far AP first stops mattering. But it
// reconnects on every STA_DISCONNECTED itself (esp_wifi_connect() enables
// that, esp_wifi_disconnect() disables it): this handler would then only
// count failures, give up with esp_wifi_disconnect(), and ignore
// WIFI_REASON_ROAMING. 11k/v/r and MBO pull in more of wpa_supplicant:
// measure flash and heap.
constexpr int kMaxRetries = 5;
int s_retry_count = 0;

// Posts a WIFI_MANAGER_EVENT. From the loop's own task: don't wait for room.
void announce(int32_t id, const char* name) {
  if (esp_event_post(WIFI_MANAGER_EVENT, id, nullptr, 0, 0) != ESP_OK) {
    ESP_LOGE(TAG, "Event queue full: dropped %s", name);
  }
}

std::string_view ssid_of(const wifi_config_t& cfg) {
  const auto* ssid = reinterpret_cast<const char*>(cfg.sta.ssid);
  // A 32-byte SSID fills the field, with no NUL.
  return {ssid, strnlen(ssid, sizeof(cfg.sta.ssid))};
}

bool same_network(const wifi_config_t& a, const wifi_config_t& b) {
  return std::memcmp(a.sta.ssid, b.sta.ssid, sizeof(a.sta.ssid)) == 0 &&
         std::memcmp(a.sta.password, b.sta.password, sizeof(a.sta.password)) ==
             0 &&
         a.sta.threshold.authmode == b.sta.threshold.authmode;
}

// Joins the network the driver has stored: the last one wifi_connect() set.
void connect_to_stored() {
  wifi_config_t cfg{};
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK ||
      ssid_of(cfg).empty()) {
    ESP_LOGI(TAG, "No saved Wi-Fi network");
    return;
  }
  const std::string_view ssid = ssid_of(cfg);
  ESP_LOGI(TAG, "Joining saved network '%.*s'", static_cast<int>(ssid.size()),
           ssid.data());
  announce(WIFI_MANAGER_CONNECTING, "WIFI_MANAGER_CONNECTING");
  esp_wifi_connect();
}

void on_connect_request(const wifi_config_t& cfg) {
  const std::string_view ssid = ssid_of(cfg);
  ESP_LOGI(TAG, "Joining '%.*s'", static_cast<int>(ssid.size()), ssid.data());
  wifi_config_t current{};
  if (esp_wifi_get_config(WIFI_IF_STA, &current) != ESP_OK ||
      !same_network(current, cfg)) {
    // Persisted by the driver (CONFIG_ESP_WIFI_NVS_ENABLED): a flash write.
    ESP_LOGI(TAG, "Saving '%.*s' as the network to join",
             static_cast<int>(ssid.size()), ssid.data());
    wifi_config_t new_cfg = cfg;
    if (esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &new_cfg);
        err != ESP_OK) {
      ESP_LOGE(TAG, "Setting the Wi-Fi config failed: %s",
               esp_err_to_name(err));
      return;
    }
  }
  s_retry_count = 0;
  announce(WIFI_MANAGER_CONNECTING, "WIFI_MANAGER_CONNECTING");
  // Its STA_DISCONNECTED isn't a failed attempt: see on_disconnected().
  esp_wifi_disconnect();
  esp_wifi_connect();
}

void on_disconnected(const wifi_event_sta_disconnected_t& event) {
  // We left: on_connect_request() is joining the new network. Leaving an AP
  // reports ASSOC_LEAVE; leaving while idle (after giving up), STA_LEAVING.
  if (event.reason == WIFI_REASON_ASSOC_LEAVE ||
      event.reason == WIFI_REASON_STA_LEAVING) {
    return;
  }
  if (s_retry_count < kMaxRetries) {
    if (s_retry_count == 0) {
      // A drop (or joining's first failure, a repeat): rejoining starts.
      announce(WIFI_MANAGER_CONNECTING, "WIFI_MANAGER_CONNECTING");
    }
    s_retry_count++;
    ESP_LOGI(TAG, "Wi-Fi disconnected (reason %d), retrying (%d/%d)...",
             event.reason, s_retry_count, kMaxRetries);
    esp_wifi_connect();
    return;
  }
  ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), giving up", event.reason);
  announce(WIFI_MANAGER_GAVE_UP, "WIFI_MANAGER_GAVE_UP");
}

void on_wifi_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t id,
                   void* data) {
  switch (id) {
    case WIFI_EVENT_STA_START:
      connect_to_stored();
      break;
    case WIFI_EVENT_STA_CONNECTED:
      ESP_LOGI(TAG, "Wi-Fi connected to AP, waiting for IP...");
      s_retry_count = 0;  // A later drop gets a full set of retries.
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      on_disconnected(*static_cast<const wifi_event_sta_disconnected_t*>(data));
      break;
    default:
      break;
  }
}

void on_request(void* /*arg*/, esp_event_base_t /*base*/, int32_t id,
                void* data) {
  if (id == kConnect) {
    on_connect_request(*static_cast<const wifi_config_t*>(data));
  }
}

}  // namespace

void wifi_init() {
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_MANAGER_REQUEST, ESP_EVENT_ANY_ID, &on_request, nullptr, nullptr));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());  // STA_START joins the saved network.
}

esp_err_t wifi_connect(std::string_view ssid, std::string_view password) {
  wifi_config_t cfg{};
  if (ssid.empty() || ssid.size() > sizeof(cfg.sta.ssid) ||
      password.size() > sizeof(cfg.sta.password)) {
    return ESP_ERR_INVALID_ARG;
  }
  // Either may fill its field: a 32-byte SSID, a 64-hex-digit PSK.
  std::ranges::copy(ssid, cfg.sta.ssid);
  std::ranges::copy(password, cfg.sta.password);
  cfg.sta.threshold.authmode =
      password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  return esp_event_post(WIFI_MANAGER_REQUEST, kConnect, &cfg, sizeof(cfg),
                        pdMS_TO_TICKS(100));
}

std::string wifi_get_ssid() {
  wifi_config_t cfg{};
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    return {};  // No Wi-Fi, e.g. under QEMU.
  }
  return std::string(ssid_of(cfg));
}

}  // namespace coprocessor
