// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include "tether/typed.h"

#include "coprocessor_rpc.h"

namespace coprocessor {

/// Joins networks and runs provisioning sessions. What comes of them is
/// System.Watch's to report (services/system_service.h).
class WifiServiceHandler final : public CoprocessorProto::wifi::Handler {
 public:
  void connect(tether::Reply<CoprocessorProto::Empty> reply,
               const CoprocessorProto::WifiConnectRequest& request) override;

  void start_provisioning(
      tether::Reply<CoprocessorProto::Empty> reply,
      const CoprocessorProto::StartProvisioningRequest& request) override;

  void stop_provisioning(tether::Reply<CoprocessorProto::Empty> reply,
                         const CoprocessorProto::Empty& request) override;
};

}  // namespace coprocessor
