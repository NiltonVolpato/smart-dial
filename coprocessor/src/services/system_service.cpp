// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/system_service.h"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "net/web_server.h"
#include "services/rpc.h"

namespace coprocessor {

namespace {
constexpr const char* TAG = "system_svc";
}

SystemServiceHandler::SystemServiceHandler() {
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC,
      [](void* self, esp_event_base_t, int32_t, void*) {
        ESP_LOGI(TAG, "Time synced");
        static_cast<SystemServiceHandler*>(self)->synced_.store(
            true, std::memory_order_relaxed);
      },
      this, &sntp_handler_));
}

SystemServiceHandler::~SystemServiceHandler() {
  esp_event_handler_instance_unregister(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC,
                                        sntp_handler_);
}

void SystemServiceHandler::get_timestamp(
    tether::Reply<CoprocessorProto::GetTimestampResponse> reply,
    const CoprocessorProto::GetTimestampRequest& /*request*/) {
  if (!synced_.load(std::memory_order_relaxed)) {
    fail_reply(reply, tether::WireStatus::UNAVAILABLE);
    return;
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  const CoprocessorProto::Timestamp timestamp(
      static_cast<uint64_t>(now.tv_sec), static_cast<uint32_t>(now.tv_usec));
  send_reply(reply, [&](flatbuffers::FlatBufferBuilder& fbb) {
    return CoprocessorProto::CreateGetTimestampResponse(fbb, &timestamp);
  });
}

void SystemServiceHandler::report_battery(
    tether::Reply<CoprocessorProto::Empty> reply,
    const CoprocessorProto::BatteryStatus& request) {
  ESP_LOGI(TAG, "Battery reported: %lu mV, %u%%, plugged=%d",
           static_cast<unsigned long>(request.millivolts()),
           static_cast<unsigned>(request.percent()), request.is_plugged());

  web_server_update_battery(&request);

  send_reply(reply, [](flatbuffers::FlatBufferBuilder& fbb) {
    return CoprocessorProto::CreateEmpty(fbb);
  });
}

}  // namespace coprocessor
