// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include "net/ipv4.h"

namespace coprocessor {

/// Whether this device has an IP address, whatever brings it: Wi-Fi on the
/// device, Ethernet under QEMU. Follows ESP-IDF's IP events (got and lost IP),
/// so modules needing to act on a change register for those themselves.

/// Starts following IP events. Requires the default event loop.
void network_init();

/// Any task.
bool network_is_up();
/// Any task. Ipv4{} while down.
Ipv4 network_ip();

}  // namespace coprocessor
