// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/wifi_service.h"

#include <string_view>

#include "esp_log.h"

#include "net/wifi_manager.h"
#include "provisioning/improv_ble.h"
#include "services/rpc.h"

namespace coprocessor {

namespace {
constexpr const char* TAG = "wifi_svc";
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

}  // namespace coprocessor
