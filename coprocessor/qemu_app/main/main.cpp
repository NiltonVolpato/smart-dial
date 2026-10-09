// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// The coprocessor under QEMU: the same app, on OpenCores Ethernet instead of
// Wi-Fi, and without BLE (see ble_stub.cpp).

#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "app.h"
#include "net/ipv4.h"
#include "sonos/gena.h"

// Written by test-qemu.sh: where the host forwards GENA NOTIFYs from speakers.
#if __has_include("sonos_callback_config.h")
#include "sonos_callback_config.h"
#endif

namespace {

constexpr const char* TAG = "qemu";

// Brings up QEMU's virtual NIC. Its IP events (IP_EVENT_ETH_GOT_IP) drive
// the app as Wi-Fi's do on the device.
void start_openeth() {
  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t* netif = esp_netif_new(&netif_config);

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&mac_config);

  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.autonego_timeout_ms = 100;
  phy_config.reset_timeout_ms = 100;
  esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth = nullptr;
  ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth));
  ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
  ESP_ERROR_CHECK(esp_eth_start(eth));
  ESP_LOGI(TAG, "OpenEth started");
}

}  // namespace

extern "C" void app_main() {
  coprocessor::app_start({.tx = 17, .rx = 16});
#ifdef SONOS_CALLBACK_HOST
  coprocessor::gena_set_callback(
      coprocessor::Ipv4::parse(SONOS_CALLBACK_HOST).value_or(coprocessor::Ipv4{}),
      SONOS_CALLBACK_PORT);
#endif
  start_openeth();
}
