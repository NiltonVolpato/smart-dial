// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

namespace coprocessor {

/// Starts the SNTP client. It syncs whenever the network is up and posts
/// NETIF_SNTP_EVENT / NETIF_SNTP_TIME_SYNC on the default event loop.
void time_sync_init();

}  // namespace coprocessor
