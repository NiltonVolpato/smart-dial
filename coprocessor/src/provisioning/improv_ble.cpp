// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "provisioning/improv_ble.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "improv.h"

#include "ble/ble.h"
#include "coprocessor_generated.h"
#include "net/ipv4.h"
#include "net/wifi_manager.h"

namespace coprocessor {

ESP_EVENT_DEFINE_BASE(PROVISIONING_EVENT);

namespace {

using CoprocessorProto::ProvisioningState;

constexpr const char* TAG = "improv";

constexpr uint32_t kDefaultWindowSeconds = 3 * 60;
constexpr uint32_t kMaxWindowSeconds = 60 * 60;
// After success, so the client can read the device's URL before BLE goes.
constexpr uint32_t kGraceMs = 10'000;
// Improv capabilities: no improv::CAPABILITY_IDENTIFY.
constexpr uint8_t kCapabilities = 0;
// GET_DEVICE_INFO's firmware name; the version comes from the app.
constexpr const char* kFirmwareName = "smart-dial";

std::atomic<bool> s_active{false};

void announce(ProvisioningState state) {
  // May run on the event loop's own task: don't wait for room.
  if (esp_event_post(PROVISIONING_EVENT, static_cast<int32_t>(state), nullptr,
                     0, 0) != ESP_OK) {
    ESP_LOGE(TAG, "Event queue full: dropped provisioning state %d",
             static_cast<int>(state));
  }
}

ble_uuid_any_t parse_uuid(const char* text) {
  ble_uuid_any_t uuid{};
  if (int rc = ble_uuid_from_str(&uuid, text); rc != 0) {
    ESP_LOGE(TAG, "Bad UUID '%s': rc=%d", text, rc);
  }
  return uuid;
}

int append(os_mbuf* om, const void* data, std::size_t size) {
  return os_mbuf_append(om, data, size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// One provisioning session: the Improv GATT service, its state and timers.
// Host task only.
class ImprovSession final : public BleProfile {
 public:
  explicit ImprovSession(uint32_t window_seconds);
  ~ImprovSession();
  ImprovSession(const ImprovSession&) = delete;
  ImprovSession& operator=(const ImprovSession&) = delete;

  // Opens the window again, from now.
  void extend(uint32_t window_seconds);
  // Ends the session, announcing `state`, and destroys it.
  void end(ProvisioningState state);

  void on_got_ip(Ipv4 ip);
  void on_gave_up();

  // BleProfile.
  [[nodiscard]] const ble_gatt_svc_def* services() const override {
    return services_.data();
  }
  void on_ready() override { advertise(); }
  void on_gap_event(const ble_gap_event& event) override;
  int on_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                     ble_gatt_access_ctxt& ctxt) override;

 private:
  void advertise();
  void on_rpc_command(ble_gatt_access_ctxt& ctxt);
  void set_state(uint8_t state, uint8_t error);
  void on_window_end();
  void on_grace_end();

  uint8_t state_ = improv::STATE_AUTHORIZED;
  uint8_t error_ = improv::ERROR_NONE;
  std::vector<uint8_t> rpc_result_;
  bool connected_ = false;

  // The GATT service, pointing into the members above and below: the session
  // must not move.
  ble_uuid_any_t service_uuid_ = parse_uuid(improv::SERVICE_UUID);
  ble_uuid_any_t status_uuid_ = parse_uuid(improv::STATUS_UUID);
  ble_uuid_any_t error_uuid_ = parse_uuid(improv::ERROR_UUID);
  ble_uuid_any_t rpc_command_uuid_ = parse_uuid(improv::RPC_COMMAND_UUID);
  ble_uuid_any_t rpc_result_uuid_ = parse_uuid(improv::RPC_RESULT_UUID);
  ble_uuid_any_t capabilities_uuid_ = parse_uuid(improv::CAPABILITIES_UUID);
  uint16_t status_handle_ = 0;
  uint16_t error_handle_ = 0;
  uint16_t rpc_command_handle_ = 0;
  uint16_t rpc_result_handle_ = 0;
  uint16_t capabilities_handle_ = 0;
  std::array<ble_gatt_chr_def, 6> characteristics_;
  std::array<ble_gatt_svc_def, 2> services_;

  BleTimer window_{tether::Callback(this, &ImprovSession::on_window_end)};
  BleTimer grace_{tether::Callback(this, &ImprovSession::on_grace_end)};
  esp_event_handler_instance_t got_ip_handler_ = nullptr;
  esp_event_handler_instance_t gave_up_handler_ = nullptr;
};

// Host task only.
std::unique_ptr<ImprovSession> s_session;

// Event loop task: hand Wi-Fi's outcome to the session, on the host task.
void on_network_event(void* /*arg*/, esp_event_base_t base, int32_t id,
                      void* data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    const Ipv4 ip{ntohl(event->ip_info.ip.addr)};
    ble_post([ip] {
      if (s_session != nullptr) s_session->on_got_ip(ip);
    });
  } else if (base == WIFI_MANAGER_EVENT && id == WIFI_MANAGER_GAVE_UP) {
    ble_post([] {
      if (s_session != nullptr) s_session->on_gave_up();
    });
  }
}

ImprovSession::ImprovSession(uint32_t window_seconds)
    : characteristics_{{
          {
              .uuid = &status_uuid_.u,
              .access_cb = ble_gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &status_handle_,
          },
          {
              .uuid = &error_uuid_.u,
              .access_cb = ble_gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &error_handle_,
          },
          {
              .uuid = &rpc_command_uuid_.u,
              .access_cb = ble_gatt_access,
              .flags = BLE_GATT_CHR_F_WRITE,
              .val_handle = &rpc_command_handle_,
          },
          {
              .uuid = &rpc_result_uuid_.u,
              .access_cb = ble_gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &rpc_result_handle_,
          },
          {
              .uuid = &capabilities_uuid_.u,
              .access_cb = ble_gatt_access,
              .flags = BLE_GATT_CHR_F_READ,
              .val_handle = &capabilities_handle_,
          },
          {.uuid = nullptr},
      }},
      services_{{
          {
              .type = BLE_GATT_SVC_TYPE_PRIMARY,
              .uuid = &service_uuid_.u,
              .characteristics = characteristics_.data(),
          },
          {.type = 0},
      }} {
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_network_event, nullptr,
      &got_ip_handler_));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_MANAGER_EVENT, WIFI_MANAGER_GAVE_UP, &on_network_event, nullptr,
      &gave_up_handler_));
  extend(window_seconds);
}

ImprovSession::~ImprovSession() {
  if (got_ip_handler_ != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          got_ip_handler_);
  }
  if (gave_up_handler_ != nullptr) {
    esp_event_handler_instance_unregister(
        WIFI_MANAGER_EVENT, WIFI_MANAGER_GAVE_UP, gave_up_handler_);
  }
}

void ImprovSession::extend(uint32_t window_seconds) {
  if (window_seconds == 0) {
    window_seconds = kDefaultWindowSeconds;
  }
  window_seconds = std::min(window_seconds, kMaxWindowSeconds);
  ESP_LOGI(TAG, "Provisioning open for %u s",
           static_cast<unsigned>(window_seconds));
  window_.start(window_seconds * 1000);
}

void ImprovSession::end(ProvisioningState state) {
  s_active = false;
  window_.stop();
  grace_.stop();
  announce(state);
  ble_release(*this);
  s_session.reset();  // Destroys this: last.
}

void ImprovSession::set_state(uint8_t state, uint8_t error) {
  const bool error_changed = error != error_;
  state_ = state;
  error_ = error;
  if (error_changed) {
    ble_gatts_chr_updated(error_handle_);
  }
  ble_gatts_chr_updated(status_handle_);
}

void ImprovSession::advertise() {
  if (connected_) {
    return;
  }
  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &service_uuid_.u128;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  if (ble_advertise(fields)) {
    ESP_LOGI(TAG, "Advertising Improv as '%s'", ble_device_name());
  }
}

void ImprovSession::on_gap_event(const ble_gap_event& event) {
  switch (event.type) {
    case BLE_GAP_EVENT_CONNECT:
      ESP_LOGI(TAG, "Client connection %s; status=%d",
               event.connect.status == 0 ? "established" : "failed",
               event.connect.status);
      connected_ = event.connect.status == 0;
      advertise();  // If it failed.
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "Client disconnected; reason=%d", event.disconnect.reason);
      connected_ = false;
      if (state_ != improv::STATE_PROVISIONED) {
        advertise();
      }
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();  // Unless a client connected.
      break;
    default:
      break;
  }
}

int ImprovSession::on_gatt_access(uint16_t /*conn_handle*/,
                                  uint16_t attr_handle,
                                  ble_gatt_access_ctxt& ctxt) {
  if (ctxt.op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (attr_handle == status_handle_) {
      return append(ctxt.om, &state_, sizeof(state_));
    }
    if (attr_handle == error_handle_) {
      return append(ctxt.om, &error_, sizeof(error_));
    }
    if (attr_handle == rpc_result_handle_) {
      return append(ctxt.om, rpc_result_.data(), rpc_result_.size());
    }
    if (attr_handle == capabilities_handle_) {
      return append(ctxt.om, &kCapabilities, sizeof(kCapabilities));
    }
  } else if (ctxt.op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
             attr_handle == rpc_command_handle_) {
    on_rpc_command(ctxt);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

void ImprovSession::on_rpc_command(ble_gatt_access_ctxt& ctxt) {
  std::vector<uint8_t> data(OS_MBUF_PKTLEN(ctxt.om));
  if (os_mbuf_copydata(ctxt.om, 0, data.size(), data.data()) != 0) {
    set_state(state_, improv::ERROR_INVALID_RPC);
    return;
  }
  const improv::ImprovCommand command = improv::parse_improv_data(data);
  switch (command.command) {
    case improv::Command::WIFI_SETTINGS:
      ESP_LOGI(TAG, "Wi-Fi settings received for '%s'", command.ssid.c_str());
      set_state(improv::STATE_PROVISIONING, improv::ERROR_NONE);
      if (wifi_connect(command.ssid, command.password) != ESP_OK) {
        on_gave_up();
      }
      break;
    case improv::Command::GET_DEVICE_INFO: {
      const esp_app_desc_t* app = esp_app_get_description();
      rpc_result_ = improv::build_rpc_response(
          improv::Command::GET_DEVICE_INFO,
          {kFirmwareName, app->version, CONFIG_IDF_TARGET, ble_device_name()});
      ble_gatts_chr_updated(rpc_result_handle_);
      break;
    }
    default:
      // Includes IDENTIFY (0x02, as GET_CURRENT_STATE): not in kCapabilities.
      ESP_LOGW(TAG, "Unsupported Improv command %d",
               static_cast<int>(command.command));
      set_state(state_, improv::ERROR_UNKNOWN_RPC);
      break;
  }
}

void ImprovSession::on_got_ip(Ipv4 ip) {
  // Only the network the client sent: not a reconnection meanwhile.
  if (state_ != improv::STATE_PROVISIONING) {
    return;
  }
  const std::string url = "http://" + ip.to_string();
  rpc_result_ =
      improv::build_rpc_response(improv::Command::WIFI_SETTINGS, {url});
  ble_gatts_chr_updated(rpc_result_handle_);
  set_state(improv::STATE_PROVISIONED, improv::ERROR_NONE);
  ESP_LOGI(TAG, "Provisioned: %s", url.c_str());
  announce(ProvisioningState::Provisioned);
  window_.stop();
  grace_.start(kGraceMs);
}

void ImprovSession::on_gave_up() {
  if (state_ != improv::STATE_PROVISIONING) {
    return;
  }
  ESP_LOGW(TAG, "Unable to connect: back to awaiting Wi-Fi settings");
  set_state(improv::STATE_AUTHORIZED, improv::ERROR_UNABLE_TO_CONNECT);
}

void ImprovSession::on_window_end() {
  ESP_LOGI(TAG, "Provisioning window timed out");
  end(ProvisioningState::TimedOut);
}

void ImprovSession::on_grace_end() { end(ProvisioningState::Inactive); }

}  // namespace

void provisioning_start(uint32_t timeout_seconds) {
  ble_post([timeout_seconds] {
    if (s_session != nullptr) {
      s_session->extend(timeout_seconds);
      return;
    }
    s_session = std::make_unique<ImprovSession>(timeout_seconds);
    if (!ble_acquire(*s_session)) {
      ESP_LOGW(TAG, "BLE is busy: no provisioning session");
      s_session.reset();
      announce(ProvisioningState::Inactive);
      return;
    }
    s_active = true;
    announce(ProvisioningState::Active);
  });
}

void provisioning_stop() {
  ble_post([] {
    if (s_session != nullptr) {
      s_session->end(ProvisioningState::Inactive);
    }
  });
}

bool provisioning_is_active() { return s_active; }

}  // namespace coprocessor
