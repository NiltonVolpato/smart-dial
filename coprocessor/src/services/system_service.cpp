// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/system_service.h"

#include <sys/time.h>

#include <bit>
#include <chrono>
#include <utility>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"

#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "provisioning/improv_ble.h"
#include "services/rpc.h"

namespace coprocessor {

namespace {

using CoprocessorProto::ProvisioningState;
using CoprocessorProto::SystemEvent;
using CoprocessorProto::SystemEventKind;
using CoprocessorProto::WifiState;
using tether::CallError;

constexpr const char* TAG = "system_svc";

// How long to wait before sending again, when the stream had no room.
constexpr std::chrono::milliseconds kSendRetry{50};

const char* name(WifiState state) {
  switch (state) {
    case WifiState::Disconnected:
      return "disconnected";
    case WifiState::Connecting:
      return "connecting";
    case WifiState::Connected:
      return "connected";
    case WifiState::Failed:
      return "failed";
  }
  return "?";
}

// The events followed, in event_handlers_'s order.
std::array<std::pair<esp_event_base_t, int32_t>, 4> followed_events() {
  return {{
      {WIFI_MANAGER_EVENT, ESP_EVENT_ANY_ID},
      {IP_EVENT, ESP_EVENT_ANY_ID},
      {PROVISIONING_EVENT, ESP_EVENT_ANY_ID},
      {NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC},
  }};
}

}  // namespace

SystemServiceHandler::SystemServiceHandler(Reactor& reactor)
    : reactor_(reactor) {
  const auto events = followed_events();
  for (std::size_t i = 0; i < events.size(); ++i) {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        events[i].first, events[i].second, &on_event, this,
        &event_handlers_[i]));
  }
}

SystemServiceHandler::~SystemServiceHandler() {
  const auto events = followed_events();
  for (std::size_t i = 0; i < events.size(); ++i) {
    esp_event_handler_instance_unregister(events[i].first, events[i].second,
                                          event_handlers_[i]);
  }
}

void SystemServiceHandler::on_event(void* self, esp_event_base_t base,
                                    int32_t id, void* data) {
  auto& handler = *static_cast<SystemServiceHandler*>(self);
  if (base == PROVISIONING_EVENT) {
    {
      std::scoped_lock lock(handler.mutex_);
      handler.provisioning_ = static_cast<ProvisioningState>(id);
      handler.changed_ |= kProvisioning;
    }
    handler.request_flush();
  } else if (base == NETIF_SNTP_EVENT) {
    ESP_LOGI(TAG, "Time synced");
    {
      std::scoped_lock lock(handler.mutex_);
      handler.time_synced_ = true;
      handler.changed_ |= kTime;
    }
    handler.request_flush();
  } else {
    handler.on_wifi_event(base, id, data);
  }
}

// SSID and signal only exist over Wi-Fi: under QEMU's Ethernet they're empty
// and 0.
void SystemServiceHandler::on_wifi_event(esp_event_base_t base, int32_t id,
                                         const void* data) {
  Wifi next;
  next.ssid = wifi_get_ssid();
  if (base == WIFI_MANAGER_EVENT) {
    next.state = id == WIFI_MANAGER_CONNECTING ? WifiState::Connecting
                                               : WifiState::Failed;
  } else if (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP) {
    // From the event, not net/network.h: its handler may not have run yet.
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    next.state = WifiState::Connected;
    next.ip = Ipv4{ntohl(event->ip_info.ip.addr)};
    wifi_ap_record_t ap_info{};
    if (id == IP_EVENT_STA_GOT_IP &&
        esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      next.rssi = ap_info.rssi;
    }
  } else if (id == IP_EVENT_STA_LOST_IP || id == IP_EVENT_ETH_LOST_IP) {
    next.state = WifiState::Disconnected;
  } else {
    return;
  }

  {
    std::scoped_lock lock(mutex_);
    // Losing the IP only matters while connected. Otherwise wifi_manager
    // already said more: it's rejoining (Connecting), or gave up (Failed,
    // which stays until the next Connecting or IP).
    if ((next.state == WifiState::Disconnected &&
         wifi_.state != WifiState::Connected) ||
        next == wifi_) {
      return;
    }
    wifi_ = std::move(next);
    ESP_LOGI(TAG, "Wi-Fi %s, ssid='%s', ip=%s, rssi=%d", name(wifi_.state),
             wifi_.ssid.c_str(), wifi_.ip.to_string().c_str(), wifi_.rssi);
    changed_ |= kWifi;
  }
  request_flush();
}

void SystemServiceHandler::watch(
    tether::Sink<CoprocessorProto::SystemEvent> sink,
    const CoprocessorProto::Empty& /*request*/) {
  ESP_LOGI(TAG, "Watch subscribed (call_id=%u)",
           static_cast<unsigned>(sink.call_id()));
  {
    std::scoped_lock lock(mutex_);
    new_sink_ = sink;
  }
  request_flush();
}

void SystemServiceHandler::request_flush() {
  if (!flush_posted_.exchange(true)) {
    reactor_.post(Callback(this, &SystemServiceHandler::run_flush));
  }
}

void SystemServiceHandler::run_flush() {
  flush_posted_ = false;
  flush();
}

void SystemServiceHandler::run_retry() {
  retry_timer_ = 0;
  flush();
}

void SystemServiceHandler::flush() {
  Wifi wifi;
  ProvisioningState provisioning{};
  {
    std::scoped_lock lock(mutex_);
    if (new_sink_) {
      // One watcher: the newest. Ending fails as Closed if the old one is
      // gone already, or as QueueFull: then its client times out.
      (void)sink_.end(tether::WireStatus::ABORTED);
      sink_ = *new_sink_;
      new_sink_.reset();
      unsent_ = kWifi | kProvisioning | (time_synced_ ? kTime : 0);
    }
    unsent_ |= changed_;
    changed_ = 0;
    wifi = wifi_;
    provisioning = provisioning_;
  }

  while (unsent_ != 0) {
    const auto kind = static_cast<Kind>(1U << std::countr_zero(unsent_));
    auto result = sink_.send([&](flatbuffers::FlatBufferBuilder& fbb) {
      switch (kind) {
        case kWifi: {
          auto status = CoprocessorProto::CreateWifiStatusDirect(
              fbb, wifi.state, wifi.ip.value, wifi.ssid.c_str(), wifi.rssi);
          return CoprocessorProto::CreateSystemEvent(
              fbb, SystemEventKind::WifiStatus, status.Union());
        }
        case kProvisioning: {
          auto status =
              CoprocessorProto::CreateProvisioningStatus(fbb, provisioning);
          return CoprocessorProto::CreateSystemEvent(
              fbb, SystemEventKind::ProvisioningStatus, status.Union());
        }
        case kTime: {
          // Read now, not at the sync: a send that waited isn't stale.
          timeval now{};
          gettimeofday(&now, nullptr);
          const CoprocessorProto::Timestamp timestamp(
              static_cast<uint64_t>(now.tv_sec),
              static_cast<uint32_t>(now.tv_usec));
          auto synced = CoprocessorProto::CreateTimeSynced(fbb, &timestamp);
          return CoprocessorProto::CreateSystemEvent(
              fbb, SystemEventKind::TimeSynced, synced.Union());
        }
      }
      return flatbuffers::Offset<SystemEvent>{};
    });
    if (!result) {
      switch (result.error()) {
        case CallError::QueueFull:
        case CallError::NoCredit:
          if (retry_timer_ == 0) {
            retry_timer_ = reactor_.after(
                kSendRetry, Callback(this, &SystemServiceHandler::run_retry));
          }
          return;
        case CallError::Closed:
          // Nobody's watching: the next Watch gets every kind anyway.
          unsent_ = 0;
          return;
        case CallError::TooLarge:
          ESP_LOGE(TAG, "Event kind %u exceeds the max payload: skipped",
                   static_cast<unsigned>(kind));
          break;
      }
    }
    unsent_ &= ~kind;
  }
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
