// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include "esp_event.h"
#include "tether/typed.h"

#include "coprocessor_rpc.h"
#include "net/ipv4.h"

namespace coprocessor {

/// Streams network and provisioning status, following IP and provisioning
/// events. Requires the default event loop before construction.
class WifiServiceHandler final : public CoprocessorProto::wifi::Handler {
 public:
  WifiServiceHandler();
  ~WifiServiceHandler();
  WifiServiceHandler(const WifiServiceHandler&) = delete;
  WifiServiceHandler& operator=(const WifiServiceHandler&) = delete;

  void connect(tether::Reply<CoprocessorProto::Empty> reply,
               const CoprocessorProto::WifiConnectRequest& request) override;

  void watch(tether::Sink<CoprocessorProto::WifiStatus> sink,
             const CoprocessorProto::Empty& request) override;

  void start_provisioning(
      tether::Reply<CoprocessorProto::Empty> reply,
      const CoprocessorProto::StartProvisioningRequest& request) override;

  void stop_provisioning(tether::Reply<CoprocessorProto::Empty> reply,
                         const CoprocessorProto::Empty& request) override;

  void watch_provisioning(
      tether::Sink<CoprocessorProto::ProvisioningStatus> sink,
      const CoprocessorProto::Empty& request) override;

  void cancelled(tether::CallId call) override;

  void reset();

 private:
  // ESP-IDF event task.
  static void on_ip_event(void* self, esp_event_base_t base, int32_t id,
                          void* data);
  static void on_provisioning_event(void* self, esp_event_base_t base,
                                    int32_t id, void* data);

  void send_status(Ipv4 ip);
  void send_provisioning(CoprocessorProto::ProvisioningState state);

  esp_event_handler_instance_t ip_handler_ = nullptr;
  esp_event_handler_instance_t provisioning_handler_ = nullptr;
  tether::Sink<CoprocessorProto::WifiStatus> wifi_sink_{};
  tether::Sink<CoprocessorProto::ProvisioningStatus> prov_sink_{};
};

}  // namespace coprocessor
