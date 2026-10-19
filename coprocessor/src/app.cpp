// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "app.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "tether/frame.h"
#include "tether/router.h"
#include "tether_idf/uart_io.h"

#include "async/reactor.h"
#include "coprocessor_rpc.h"
#include "net/connection_permits.h"
#include "net/network.h"
#include "net/time_sync.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "provisioning/improv_ble.h"
#include "services/sonos_service.h"
#include "services/system_service.h"
#include "services/wifi_service.h"
#include "sonos/gena.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "app";

constexpr auto kBaudRate =
    static_cast<uint32_t>(CoprocessorProto::Limits::BaudRate);
constexpr auto kMaxPayload =
    static_cast<std::size_t>(CoprocessorProto::Limits::MaxPayload);
// Clients size their receive buffers from the schema's MaxFrame.
static_assert(tether::max_wire_size(kMaxPayload) - 1 ==
                  static_cast<std::size_t>(CoprocessorProto::Limits::MaxFrame),
              "Limits.MaxFrame in schemas/coprocessor.fbs is out of date");
constexpr std::size_t kMaxCalls = 12;
constexpr std::size_t kMaxStreams = 8;
// Outgoing HTTP connections at once. lwIP has 16 sockets (sdkconfig): the
// web server takes 6 (listening, control, 4 clients), these 6, Sonos
// discovery 1, 3 spare.
constexpr std::size_t kMaxConnections = 6;
// Sonos discoveries at once: SonosServiceHandler runs one at a time.
constexpr std::size_t kMaxDiscoveries = 1;
// GENA subscriptions at once: WatchPlayback has two per stream.
constexpr std::size_t kMaxSubscriptions = 2 * kMaxStreams;
// Calls' HttpClients, and one per background UNSUBSCRIBE.
constexpr std::size_t kMaxHttpClients =
    kMaxCalls * SonosServiceHandler::kMaxHttpClientsPerCall + kMaxSubscriptions;
// No latest-value buffers: nothing calls Sink::set_latest.
constexpr std::size_t kMaxLatest = 0;

using CoprocessorServer =
    tether::StaticServer<kMaxPayload, kMaxCalls, kMaxLatest>;

uint32_t new_boot_id() { return esp_random() | 1; }

struct App {
  explicit App(TetherPins pins)
      : io{{
            .port = UART_NUM_1,
            .tx_pin = pins.tx,
            .rx_pin = pins.rx,
            .baud_rate = kBaudRate,
        }} {
    router.add(wifi_service);
    router.add(system_service);
    router.add(sonos_service);
  }

  // First: handlers below keep references to these. Sized from what tether
  // bounds (calls, streams):
  // - inbox: a start per call and per UNSUBSCRIBE, per stream a flush, a
  //   ready() and a finish, and System.Watch's flush;
  // - watches: a socket per connection and per discovery;
  // - timers: one per HttpClient, and per stream a renewal (a discovery's
  //   query schedule).
  Reactor reactor{{
      .inbox = kMaxCalls + kMaxSubscriptions + 3 * kMaxStreams + 1,
      .watches = kMaxConnections + kMaxDiscoveries,
      .timers = kMaxHttpClients + kMaxStreams,
  }};
  ConnectionPermits connections{kMaxConnections, kMaxHttpClients};
  GenaRegistry gena{kMaxSubscriptions};
  tether::idf::UartIo io;
  CoprocessorServer server{
      new_boot_id(), {.baud_rate = kBaudRate}, kMaxStreams};
  tether::StaticRouter<CoprocessorProto::kServer.size()> router{
      server, CoprocessorProto::kServer};
  WifiServiceHandler wifi_handler;
  CoprocessorProto::wifi::Service wifi_service{wifi_handler};
  SystemServiceHandler system_handler{reactor};
  CoprocessorProto::system::Service system_service{system_handler};
  SonosServiceHandler sonos_handler{reactor, connections, gena};
  CoprocessorProto::sonos::Service sonos_service{sonos_handler};
};

void heartbeat(TimerHandle_t timer) {
  const App& app = *static_cast<const App*>(pvTimerGetTimerID(timer));
  const auto uptime_sec =
      static_cast<unsigned long>(esp_timer_get_time() / 1'000'000);
  const auto free_heap_kb =
      static_cast<unsigned long>(esp_get_free_heap_size() / 1024);
  // Fragmentation shows as the largest block shrinking while the total holds.
  const auto largest_block_kb = static_cast<unsigned long>(
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
  const auto min_free_heap_kb =
      static_cast<unsigned long>(esp_get_minimum_free_heap_size() / 1024);
  const std::string ssid = wifi_get_ssid();
  int8_t rssi = 0;
  wifi_ap_record_t ap_info{};
  if (network_is_up() && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }
  ESP_LOGI(TAG,
           "[Heartbeat] uptime=%lus, heap=%luKB (largest block %luKB, min "
           "%luKB), network=%s (ip=%s, "
           "ssid='%s', rssi=%d), provisioning=%s",
           uptime_sec, free_heap_kb, largest_block_kb, min_free_heap_kb,
           network_is_up() ? "up" : "down", network_ip().to_string().c_str(),
           ssid.c_str(), rssi, provisioning_is_active() ? "active" : "off");
  // Read without the server's lock: counters, a stale one is fine here.
  const tether::idf::UartStats& uart = app.io.stats();
  const tether::LinkStats& link = app.server.link().stats();
  ESP_LOGI(TAG,
           "[Heartbeat] uart: overflows=%lu, buffer full=%lu, line "
           "errors=%lu; link: frames tx=%lu rx=%lu, retransmits=%lu, crc "
           "errors=%lu, cobs errors=%lu",
           static_cast<unsigned long>(uart.overflows),
           static_cast<unsigned long>(uart.buffer_full),
           static_cast<unsigned long>(uart.line_errors),
           static_cast<unsigned long>(link.frames_tx),
           static_cast<unsigned long>(link.frames_rx),
           static_cast<unsigned long>(link.retransmits),
           static_cast<unsigned long>(link.crc_errors),
           static_cast<unsigned long>(link.cobs_errors));
  // The S3's acks: how long it took beyond each frame's time on the wire.
  const tether::AckWaits& acks = link.ack_waits;
  const auto& b = acks.buckets;
  ESP_LOGI(
      TAG,
      "[Heartbeat] S3 acks: %lu, mean %lu ms, max %lu ms; ms <1:%lu "
      "<2:%lu <4:%lu <8:%lu <16:%lu <32:%lu >=32:%lu",
      static_cast<unsigned long>(acks.count),
      static_cast<unsigned long>(acks.count != 0 ? acks.total_ms / acks.count
                                                 : 0),
      static_cast<unsigned long>(acks.max_ms), static_cast<unsigned long>(b[0]),
      static_cast<unsigned long>(b[1]), static_cast<unsigned long>(b[2]),
      static_cast<unsigned long>(b[3]), static_cast<unsigned long>(b[4]),
      static_cast<unsigned long>(b[5]), static_cast<unsigned long>(b[6]));
}

void reactor_task(void* arg) {
  static_cast<App*>(arg)->reactor.run();
  vTaskDelete(nullptr);
}

// Serves one link, then reboots: everything here is per S3 session, and a
// reboot is the one reset that misses nothing (handlers, subscriptions, the
// heap). The S3 reboots when its link ends too.
[[noreturn]] void tether_io(void* arg) {
  App& app = *static_cast<App*>(arg);
  ESP_LOGI(TAG, "Tether link serving, boot id %08lx",
           static_cast<unsigned long>(app.server.link().boot_id()));
  const tether::LinkState state = app.io.serve(app.server);
  ESP_LOGW(TAG, "Tether link ended (%s): rebooting",
           state == tether::LinkState::PeerRebooted ? "the S3 rebooted"
                                                    : "the S3 is lost");
  esp_restart();
}

}  // namespace

void app_start(TetherPins pins) {
  // NVS keeps Wi-Fi credentials.
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Process-wide networking and event loop; modules register handlers on it.
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // The reactor's wake-up channel.
  const esp_vfs_eventfd_config_t eventfd_config =
      ESP_VFS_EVENTD_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

  network_init();

  static App app{pins};
  web_server_init(app.gena);

  xTaskCreate(reactor_task, "reactor", 6144, &app, 5, nullptr);
  // Above the app's tasks, so ACKs go out promptly.
  xTaskCreate(tether_io, "tether_io", 4096, &app, 10, nullptr);

  time_sync_init();

  TimerHandle_t heartbeat_timer =
      xTimerCreate("heartbeat", pdMS_TO_TICKS(10'000), pdTRUE, &app, heartbeat);
  if (heartbeat_timer != nullptr) {
    xTimerStart(heartbeat_timer, 0);
  }
}

}  // namespace coprocessor
