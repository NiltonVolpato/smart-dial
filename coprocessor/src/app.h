// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

namespace coprocessor {

/// Where the Tether link's UART is wired.
struct TetherPins {
  int tx;
  int rx;
};

/// Starts the coprocessor without a network: storage, the default event loop,
/// the Tether link and its services, the reactor, SNTP, the Sonos controller
/// and the web server. The caller then brings a network up (Wi-Fi on the
/// device, Ethernet under QEMU): everything else follows its IP events.
void app_start(TetherPins pins);

}  // namespace coprocessor
