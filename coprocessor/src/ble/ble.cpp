// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "ble/ble.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "ble";

// The host's state. Host task only, after ble_init().
struct Host {
  bool synced = false;
  uint8_t own_addr_type = 0;
  BleProfile* owner = nullptr;
  std::array<uint16_t, CONFIG_BT_NIMBLE_MAX_CONNECTIONS> connections{};
  std::size_t connection_count = 0;
};
Host s_host;

// Profiles' services come and go as dynamic services: ble_gatts_reset() and
// ble_gatts_start() can't do it, as re-registering GAP and GATT grows
// NimBLE's attribute pools every time (ble_gatts_count_cfg() only adds).
//
// But NimBLE sizes those pools when the host starts, from the services
// counted by then, and won't advertise for connections without a free
// client configuration for every notifying characteristic. So reserve room
// for the largest profile, Improv: one service of 5 characteristics, 3 of
// them notifying. It costs about 100 B of heap from boot; raise it when a
// bigger profile comes.
constexpr std::size_t kReservedCharacteristics = 5;
constexpr std::size_t kReservedNotifying = 3;

void reserve_profile_room() {
  static constexpr ble_uuid16_t kUuid = BLE_UUID16_INIT(0xFFFF);
  std::array<ble_gatt_chr_def, kReservedCharacteristics + 1> characteristics{};
  for (std::size_t i = 0; i < kReservedCharacteristics; i++) {
    characteristics[i] = {
        .uuid = &kUuid.u,
        .access_cb = ble_gatt_access,  // Required, though never called.
        .flags = static_cast<ble_gatt_chr_flags>(
            i < kReservedNotifying ? BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY
                                   : BLE_GATT_CHR_F_READ),
    };
  }
  const std::array<ble_gatt_svc_def, 2> services{{
      {
          .type = BLE_GATT_SVC_TYPE_PRIMARY,
          .uuid = &kUuid.u,
          .characteristics = characteristics.data(),
      },
      {.type = 0},
  }};
  // Counted, never registered.
  if (int rc = ble_gatts_count_cfg(services.data()); rc != 0) {
    ESP_LOGE(TAG, "Reserving room for profiles failed: rc=%d", rc);
  }
}

void remove_services(const ble_gatt_svc_def* services) {
  for (const ble_gatt_svc_def* svc = services; svc->type != 0; svc++) {
    if (int rc = ble_gatts_delete_svc(svc->uuid); rc != 0) {
      ESP_LOGE(TAG, "Removing a service failed: rc=%d", rc);
    }
  }
}

int on_gap_event(ble_gap_event* event, void* /*arg*/) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0 &&
          s_host.connection_count < s_host.connections.size()) {
        s_host.connections[s_host.connection_count++] =
            event->connect.conn_handle;
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT: {
      auto end = s_host.connections.begin() + s_host.connection_count;
      if (std::remove(s_host.connections.begin(), end,
                      event->disconnect.conn.conn_handle) != end) {
        s_host.connection_count--;
      }
      break;
    }
    default:
      break;
  }
  if (s_host.owner != nullptr) {
    s_host.owner->on_gap_event(*event);
  }
  return 0;
}

void on_sync() {
  if (int rc = ble_hs_util_ensure_addr(0); rc != 0) {
    ESP_LOGE(TAG, "No BLE address: rc=%d", rc);
    return;
  }
  if (int rc = ble_hs_id_infer_auto(0, &s_host.own_addr_type); rc != 0) {
    ESP_LOGE(TAG, "Inferring the address type failed: rc=%d", rc);
    return;
  }
  s_host.synced = true;
  if (s_host.owner != nullptr) {
    s_host.owner->on_ready();
  }
}

void on_reset(int reason) {
  ESP_LOGW(TAG, "Host reset: reason=%d", reason);
  s_host.synced = false;
  s_host.connection_count = 0;
}

void host_task(void* /*param*/) {
  nimble_port_run();  // Returns after nimble_port_stop().
  nimble_port_freertos_deinit();
}

void run_post(ble_npl_event* event) {
  std::unique_ptr<detail::BlePost> post(
      static_cast<detail::BlePost*>(ble_npl_event_get_arg(event)));
  post->run();
  ble_npl_event_deinit(&post->event);
}

}  // namespace

void ble_init() {
  ESP_ERROR_CHECK(nimble_port_init());
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  reserve_profile_room();
  std::array<uint8_t, 6> mac{};
  ESP_ERROR_CHECK(esp_read_mac(mac.data(), ESP_MAC_WIFI_STA));
  std::array<char, 16> name{};
  snprintf(name.data(), name.size(), "Smart Dial %02X%02X", mac[4], mac[5]);
  if (int rc = ble_svc_gap_device_name_set(name.data()); rc != 0) {
    ESP_LOGE(TAG, "Setting the device name failed: rc=%d", rc);
  }
  ESP_LOGI(TAG, "Device name: %s", name.data());

  nimble_port_freertos_init(host_task);
}

namespace detail {

void ble_post(std::unique_ptr<BlePost> post) {
  ble_npl_event_init(&post->event, &run_post, post.get());
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &post.release()->event);
}

}  // namespace detail

bool ble_acquire(BleProfile& profile) {
  if (!s_host.synced || s_host.owner != nullptr) {
    return false;
  }
  if (int rc = ble_gatts_add_dynamic_svcs(profile.services()); rc != 0) {
    ESP_LOGE(TAG, "Registering the profile's services failed: rc=%d", rc);
    return false;
  }
  s_host.owner = &profile;
  profile.on_ready();
  return true;
}

void ble_release(BleProfile& profile) {
  if (s_host.owner != &profile) {
    return;
  }
  s_host.owner = nullptr;  // Its peers' events stop here.
  ble_gap_adv_stop();
  remove_services(profile.services());
  for (std::size_t i = 0; i < s_host.connection_count; i++) {
    ble_gap_terminate(s_host.connections[i], BLE_ERR_REM_USER_CONN_TERM);
  }
}

bool ble_advertise(const ble_hs_adv_fields& fields) {
  if (s_host.owner == nullptr || !s_host.synced) {
    return false;
  }
  if (int rc = ble_gap_adv_set_fields(&fields); rc != 0) {
    ESP_LOGE(TAG, "Setting the advertising fields failed: rc=%d", rc);
    return false;
  }
  ble_hs_adv_fields response{};
  const char* name = ble_svc_gap_device_name();
  response.name = reinterpret_cast<const uint8_t*>(name);
  response.name_len = strlen(name);
  response.name_is_complete = 1;
  if (int rc = ble_gap_adv_rsp_set_fields(&response); rc != 0) {
    ESP_LOGE(TAG, "Setting the scan response failed: rc=%d", rc);
    return false;
  }
  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  const int rc =
      ble_gap_adv_start(s_host.own_addr_type, nullptr, BLE_HS_FOREVER, &params,
                        on_gap_event, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGE(TAG, "Starting advertising failed: rc=%d", rc);
    return false;
  }
  return true;
}

int ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                    ble_gatt_access_ctxt* ctxt, void* /*arg*/) {
  if (s_host.owner == nullptr) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  return s_host.owner->on_gatt_access(conn_handle, attr_handle, *ctxt);
}

const char* ble_device_name() { return ble_svc_gap_device_name(); }

BleTimer::BleTimer(Callback callback) : callback_(callback) {
  ble_npl_callout_init(&callout_, nimble_port_get_dflt_eventq(), &fire, this);
}

BleTimer::~BleTimer() {
  ble_npl_callout_stop(&callout_);  // Also unqueues a fired, unhandled one.
  ble_npl_callout_deinit(&callout_);
}

void BleTimer::start(uint32_t ms) {
  ble_npl_callout_reset(&callout_, ble_npl_time_ms_to_ticks32(ms));
}

void BleTimer::stop() { ble_npl_callout_stop(&callout_); }

void BleTimer::fire(ble_npl_event* event) {
  static_cast<BleTimer*>(ble_npl_event_get_arg(event))->callback_();
}

}  // namespace coprocessor
