// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "net/network.h"

#include <atomic>
#include <cstdint>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "network";

std::atomic<uint32_t> s_ip{0};  // An Ipv4's value; 0 while down.

void on_ip_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t id,
                 void* data) {
  switch (id) {
    case IP_EVENT_STA_GOT_IP:
    case IP_EVENT_ETH_GOT_IP: {
      const auto* event = static_cast<const ip_event_got_ip_t*>(data);
      const Ipv4 ip{ntohl(event->ip_info.ip.addr)};  // Network byte order.
      s_ip.store(ip.value, std::memory_order_relaxed);
      ESP_LOGI(TAG, "Up: %s", ip.to_string().c_str());
      break;
    }
    case IP_EVENT_STA_LOST_IP:
    case IP_EVENT_ETH_LOST_IP:
      s_ip.store(0, std::memory_order_relaxed);
      ESP_LOGI(TAG, "Down");
      break;
    default:
      break;
  }
}

}  // namespace

void network_init() {
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, nullptr, nullptr));
}

bool network_is_up() { return s_ip.load(std::memory_order_relaxed) != 0; }

Ipv4 network_ip() { return Ipv4{s_ip.load(std::memory_order_relaxed)}; }

}  // namespace coprocessor
