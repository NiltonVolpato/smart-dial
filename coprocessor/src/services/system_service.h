// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>

#include "esp_event.h"
#include "tether/typed.h"

#include "coprocessor_rpc.h"

namespace coprocessor {

/// Requires the default event loop to exist before construction.
class SystemServiceHandler final : public CoprocessorProto::system::Handler {
 public:
  SystemServiceHandler();
  ~SystemServiceHandler();
  SystemServiceHandler(const SystemServiceHandler&) = delete;
  SystemServiceHandler& operator=(const SystemServiceHandler&) = delete;

  void get_timestamp(
      tether::Reply<CoprocessorProto::GetTimestampResponse> reply,
      const CoprocessorProto::GetTimestampRequest& request) override;

  void report_battery(tether::Reply<CoprocessorProto::Empty> reply,
                      const CoprocessorProto::BatteryStatus& request) override;

 private:
  // Set from the event loop task, read from the Tether task.
  std::atomic<bool> synced_{false};
  esp_event_handler_instance_t sntp_handler_ = nullptr;
};

}  // namespace coprocessor
