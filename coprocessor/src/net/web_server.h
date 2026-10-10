// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

namespace CoprocessorProto {
struct BatteryStatus;
}  // namespace CoprocessorProto

namespace coprocessor {

class GenaRegistry;

/// Serves the dashboard (HTTP and WebSocket, port 80) and speakers' GENA
/// NOTIFYs (to `gena`'s listeners) whenever there's an IP: starts and stops
/// itself on IP events. Requires the default event loop.
void web_server_init(GenaRegistry& gena);

/// Updates cached battery state and broadcasts real-time updates to connected
/// WebSocket clients.
void web_server_update_battery(const CoprocessorProto::BatteryStatus* status);

}  // namespace coprocessor
