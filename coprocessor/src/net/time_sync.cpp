// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "net/time_sync.h"

#include "esp_netif_sntp.h"

namespace coprocessor {

void time_sync_init() {
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
}

}  // namespace coprocessor
