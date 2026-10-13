// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "esp_event.h"
#include "tether/typed.h"

#include "async/reactor.h"
#include "coprocessor_rpc.h"
#include "net/ipv4.h"

namespace coprocessor {

/// System.Watch and battery reports.
///
/// It follows Wi-Fi, IP, provisioning and SNTP events from construction on,
/// keeping the latest state of each kind. The one watcher (a new Watch ends
/// the previous one) gets every kind first, then each kind that changes:
/// while it hasn't caught up, a kind changing again only replaces what's owed.
/// Sends run on the reactor, which retries those finding no room.
///
/// Requires the default event loop before construction, and outlives the
/// reactor's callbacks (it lives as long as the app).
class SystemServiceHandler final : public CoprocessorProto::system::Handler {
 public:
  explicit SystemServiceHandler(Reactor& reactor);
  ~SystemServiceHandler();
  SystemServiceHandler(const SystemServiceHandler&) = delete;
  SystemServiceHandler& operator=(const SystemServiceHandler&) = delete;

  void watch(tether::Sink<CoprocessorProto::SystemEvent> sink,
             const CoprocessorProto::Empty& request) override;

  void report_battery(tether::Reply<CoprocessorProto::Empty> reply,
                      const CoprocessorProto::BatteryStatus& request) override;

 private:
  // A kind of event, as a bit: masks of them say what to send. Sent in this
  // order.
  enum Kind : uint8_t {
    kWifi = 1 << 0,
    kProvisioning = 1 << 1,
    kTime = 1 << 2,
  };

  struct Wifi {
    CoprocessorProto::WifiState state =
        CoprocessorProto::WifiState::Disconnected;
    Ipv4 ip;
    int16_t rssi = 0;
    std::string ssid;

    bool operator==(const Wifi&) const = default;
  };

  // Event loop task.
  static void on_event(void* self, esp_event_base_t base, int32_t id,
                       void* data);
  void on_wifi_event(esp_event_base_t base, int32_t id, const void* data);

  // Any task: has the reactor run flush(), once however many changes come
  // before it does. Also sink_'s on_ready: a send that found no room may go
  // through now.
  void request_flush();

  // Reactor.
  void run_flush();
  void flush();

  Reactor& reactor_;
  std::array<esp_event_handler_instance_t, 4> event_handlers_{};

  // Written on the event loop and Tether I/O tasks, taken by the reactor.
  std::mutex mutex_;
  Wifi wifi_;  // Guarded by mutex_.
  CoprocessorProto::ProvisioningState provisioning_ =
      CoprocessorProto::ProvisioningState::Inactive;  // Guarded by mutex_.
  bool time_synced_ = false;                          // Guarded by mutex_.
  uint8_t changed_ = 0;  // Kinds changed since flush(). Guarded by mutex_.
  // A Watch flush() hasn't taken yet. Guarded by mutex_.
  std::optional<tether::Sink<CoprocessorProto::SystemEvent>> new_sink_;

  std::atomic<bool> flush_posted_{false};

  // Reactor only.
  tether::Sink<CoprocessorProto::SystemEvent> sink_;  // Closed: no watcher.
  uint8_t unsent_ = 0;                                // Kinds owed to sink_.
};

}  // namespace coprocessor
