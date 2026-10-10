// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/wifi_service.h"

#include <string>
#include <string_view>

#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"

#include "net/ipv4.h"
#include "net/network.h"
#include "net/wifi_manager.h"
#include "provisioning/improv_ble.h"
#include "services/rpc.h"

namespace coprocessor {

namespace {
constexpr const char* TAG = "wifi_svc";
}

WifiServiceHandler::WifiServiceHandler() {
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, this, &ip_handler_));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      PROVISIONING_EVENT, ESP_EVENT_ANY_ID, &on_provisioning_event, this,
      &provisioning_handler_));
}

WifiServiceHandler::~WifiServiceHandler() {
  esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                        ip_handler_);
  esp_event_handler_instance_unregister(PROVISIONING_EVENT, ESP_EVENT_ANY_ID,
                                        provisioning_handler_);
}

void WifiServiceHandler::on_ip_event(void* self, esp_event_base_t /*base*/,
                                     int32_t id, void* data) {
  auto* handler = static_cast<WifiServiceHandler*>(self);
  switch (id) {
    case IP_EVENT_STA_GOT_IP:
    case IP_EVENT_ETH_GOT_IP: {
      // From the event, not net/network.h: its handler may not have run yet.
      const auto* event = static_cast<const ip_event_got_ip_t*>(data);
      handler->send_status(Ipv4{ntohl(event->ip_info.ip.addr)});
      break;
    }
    case IP_EVENT_STA_LOST_IP:
    case IP_EVENT_ETH_LOST_IP:
      handler->send_status(Ipv4{});
      break;
    default:
      break;
  }
}

void WifiServiceHandler::on_provisioning_event(void* self,
                                               esp_event_base_t /*base*/,
                                               int32_t id, void* /*data*/) {
  static_cast<WifiServiceHandler*>(self)->send_provisioning(
      static_cast<CoprocessorProto::ProvisioningState>(id));
}

// Down if `ip` is none. SSID and signal only exist over Wi-Fi: under QEMU's
// Ethernet they're empty and 0.
void WifiServiceHandler::send_status(Ipv4 ip) {
  const bool up = ip != Ipv4{};
  const std::string ssid = wifi_get_ssid();
  int16_t rssi = 0;
  wifi_ap_record_t ap_info{};
  if (up && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }
  ESP_LOGI(TAG, "Status: %s, ssid='%s', ip=%s, rssi=%d", up ? "up" : "down",
           ssid.c_str(), ip.to_string().c_str(), rssi);
  if (!send_item(wifi_sink_, [&](flatbuffers::FlatBufferBuilder& fbb) {
        return CoprocessorProto::CreateWifiStatusDirect(fbb, up, ip.value,
                                                        ssid.c_str(), rssi);
      })) {
    wifi_sink_ = {};
  }
}

void WifiServiceHandler::send_provisioning(
    CoprocessorProto::ProvisioningState state) {
  if (!send_item(prov_sink_, [&](flatbuffers::FlatBufferBuilder& fbb) {
        return CoprocessorProto::CreateProvisioningStatus(fbb, state);
      })) {
    prov_sink_ = {};
  }
}

void WifiServiceHandler::connect(
    tether::Reply<CoprocessorProto::Empty> reply,
    const CoprocessorProto::WifiConnectRequest& request) {
  std::string_view ssid;
  if (request.ssid()) {
    ssid = request.ssid()->string_view();
  }
  std::string_view password;
  if (request.password()) {
    password = request.password()->string_view();
  }

  ESP_LOGI(TAG, "Connect requested for SSID: '%.*s'",
           static_cast<int>(ssid.size()), ssid.data());
  if (esp_err_t err = wifi_connect(ssid, password); err != ESP_OK) {
    ESP_LOGW(TAG, "Connect rejected: %s", esp_err_to_name(err));
    fail_reply(reply, err == ESP_ERR_INVALID_ARG
                          ? tether::WireStatus::INVALID_ARGUMENT
                          : tether::WireStatus::UNAVAILABLE);
    return;
  }

  send_reply(reply, [](flatbuffers::FlatBufferBuilder& fbb) {
    return CoprocessorProto::CreateEmpty(fbb);
  });
}

void WifiServiceHandler::watch(tether::Sink<CoprocessorProto::WifiStatus> sink,
                               const CoprocessorProto::Empty& /*request*/) {
  ESP_LOGI(TAG, "Watch Wi-Fi subscribed (call_id=%u)",
           static_cast<unsigned>(sink.call_id()));
  wifi_sink_ = sink;
  send_status(network_ip());
}

void WifiServiceHandler::start_provisioning(
    tether::Reply<CoprocessorProto::Empty> reply,
    const CoprocessorProto::StartProvisioningRequest& request) {
  uint32_t timeout_seconds = request.timeout_seconds();
  ESP_LOGI(TAG, "Start provisioning requested (timeout=%u s)",
           static_cast<unsigned>(timeout_seconds));
  provisioning_start(timeout_seconds);

  send_reply(reply, [](flatbuffers::FlatBufferBuilder& fbb) {
    return CoprocessorProto::CreateEmpty(fbb);
  });
}

void WifiServiceHandler::stop_provisioning(
    tether::Reply<CoprocessorProto::Empty> reply,
    const CoprocessorProto::Empty& /*request*/) {
  ESP_LOGI(TAG, "Stop provisioning requested");
  provisioning_stop();

  send_reply(reply, [](flatbuffers::FlatBufferBuilder& fbb) {
    return CoprocessorProto::CreateEmpty(fbb);
  });
}

void WifiServiceHandler::watch_provisioning(
    tether::Sink<CoprocessorProto::ProvisioningStatus> sink,
    const CoprocessorProto::Empty& /*request*/) {
  ESP_LOGI(TAG, "Watch provisioning subscribed (call_id=%u)",
           static_cast<unsigned>(sink.call_id()));
  prov_sink_ = sink;

  auto state = provisioning_is_active()
                   ? CoprocessorProto::ProvisioningState::Active
                   : CoprocessorProto::ProvisioningState::Inactive;

  send_provisioning(state);
}

void WifiServiceHandler::cancelled(tether::CallId call) {
  ESP_LOGI(TAG, "Call %u cancelled", static_cast<unsigned>(call));
  if (wifi_sink_.call_id() == call) {
    wifi_sink_ = {};
  }
  if (prov_sink_.call_id() == call) {
    prov_sink_ = {};
  }
}

void WifiServiceHandler::reset() {
  wifi_sink_ = {};
  prov_sink_ = {};
}

}  // namespace coprocessor
