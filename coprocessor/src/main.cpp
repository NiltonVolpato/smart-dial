// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "app.h"
#include "esp_log.h"

#include "net/wifi_manager.h"
#include "provisioning/ble_improv.h"

extern "C" void app_main() {
  coprocessor::app_start({.tx = 23, .rx = 18});
  coprocessor::ble_init();
  coprocessor::wifi_init();
  // Every module is running; the network comes up later, from events.
  // `just coprocessor::monitor` waits for this line.
  ESP_LOGI("main", "Co-processor initialized");
}
