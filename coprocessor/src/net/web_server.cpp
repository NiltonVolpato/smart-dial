// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "net/web_server.h"

#include <sys/time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "format.h"
#include "heap_log.h"

#include "coprocessor_generated.h"
#include "net/network.h"
#include "net/wifi_manager.h"
#include "sonos/gena.h"

namespace {

constexpr const char* TAG = "web_server";

// Part of lwIP's socket budget: see app.cpp.
constexpr int kMaxOpenSockets = 4;

// Started and stopped on the event loop task (IP events); broadcasts come
// from tether_io: g_server_mutex keeps the handle from being freed under them.
std::mutex g_server_mutex;
httpd_handle_t g_server = nullptr;
coprocessor::GenaRegistry* g_gena = nullptr;  // Set by web_server_init().

std::atomic<uint32_t> g_battery_millivolts{0};
std::atomic<int32_t> g_battery_percent{-1};
std::atomic<bool> g_battery_plugged{false};

constexpr std::string_view INDEX_HTML = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Dial Dashboard</title>
  <style>
    :root {
      --bg: #090d16;
      --card-bg: #131b2e;
      --card-border: #1e293b;
      --text: #f8fafc;
      --text-muted: #94a3b8;
      --accent: #38bdf8;
      --green: #10b981;
      --red: #ef4444;
      --amber: #f59e0b;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 24px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }
    .container { width: 100%; max-width: 720px; }
    header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }
    h1 { font-size: 22px; font-weight: 700; color: var(--text); }
    .badge { display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 9999px; font-size: 12px; font-weight: 600; }
    .badge-live { background: rgba(16, 185, 129, 0.15); color: var(--green); border: 1px solid var(--green); }
    .badge-connecting { background: rgba(245, 158, 11, 0.15); color: var(--amber); border: 1px solid var(--amber); }
    .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; margin-bottom: 24px; }
    .card { background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 16px; padding: 20px; display: flex; flex-direction: column; gap: 14px; box-shadow: 0 4px 20px rgba(0,0,0,0.3); }
    .card-header { display: flex; justify-content: space-between; align-items: center; }
    .card-title { font-size: 14px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.8px; color: var(--text-muted); }
    .stat-main { font-size: 32px; font-weight: 800; color: var(--text); }
    .stat-sub { font-size: 14px; color: var(--text-muted); }
    .progress-bar-bg { width: 100%; height: 8px; background: rgba(255,255,255,0.08); border-radius: 4px; overflow: hidden; margin-top: 4px; }
    .progress-bar-fill { height: 100%; background: var(--green); transition: width 0.3s ease; }
    .info-row { display: flex; justify-content: space-between; font-size: 14px; padding: 6px 0; border-bottom: 1px solid rgba(255,255,255,0.04); }
    .info-row:last-child { border-bottom: none; }
    .info-label { color: var(--text-muted); }
    .info-val { font-weight: 600; color: var(--text); }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <h1>Waveshare Smart Dial</h1>
      <div id="ws-badge" class="badge badge-connecting">
        <span class="dot"></span>
        <span id="ws-text">Connecting...</span>
      </div>
    </header>

    <div class="grid">
      <!-- Battery Card -->
      <div class="card">
        <div class="card-header">
          <span class="card-title">Battery & Power</span>
          <span id="bat-plugged" class="badge" style="background: rgba(56,189,248,0.15); color: var(--accent);">--</span>
        </div>
        <div>
          <div id="bat-pct" class="stat-main">--%</div>
          <div id="bat-mv" class="stat-sub">-- mV</div>
          <div class="progress-bar-bg">
            <div id="bat-bar" class="progress-bar-fill" style="width: 0%;"></div>
          </div>
        </div>
      </div>

      <!-- Wi-Fi Card -->
      <div class="card">
        <div class="card-header">
          <span class="card-title">Wi-Fi Network</span>
          <span id="wifi-status" class="badge badge-live">Connected</span>
        </div>
        <div class="info-row">
          <span class="info-label">SSID</span>
          <span id="wifi-ssid" class="info-val">--</span>
        </div>
        <div class="info-row">
          <span class="info-label">IP Address</span>
          <span id="wifi-ip" class="info-val">--</span>
        </div>
        <div class="info-row">
          <span class="info-label">Signal (RSSI)</span>
          <span id="wifi-rssi" class="info-val">-- dBm</span>
        </div>
      </div>

      <!-- System Resources Card -->
      <div class="card">
        <div class="card-header">
          <span class="card-title">Co-Processor Memory</span>
        </div>
        <div class="info-row">
          <span class="info-label">Free Heap</span>
          <span id="sys-heap" class="info-val">-- KB</span>
        </div>
        <div class="info-row">
          <span class="info-label">Minimum Free Heap</span>
          <span id="sys-min-heap" class="info-val">-- KB</span>
        </div>
        <div class="info-row">
          <span class="info-label">Uptime</span>
          <span id="sys-uptime" class="info-val">--</span>
        </div>
      </div>

      <!-- Synchronized Time Card -->
      <div class="card">
        <div class="card-header">
          <span class="card-title">Network Time (SNTP)</span>
        </div>
        <div id="time-formatted" class="stat-main" style="font-size: 20px; font-weight: 600;">--</div>
        <div class="info-row">
          <span class="info-label">Epoch Seconds</span>
          <span id="time-epoch" class="info-val">--</span>
        </div>
      </div>
    </div>
  </div>

  <script>
    function updateUI(data) {
      if (data.battery) {
        const b = data.battery;
        document.getElementById('bat-pct').innerText = b.is_plugged ? '⚡ Charging' : (b.percent >= 0 ? `${b.percent}%` : '--%');
        document.getElementById('bat-mv').innerText = b.millivolts > 0 ? `${b.millivolts} mV` : '-- mV';
        const bar = document.getElementById('bat-bar');
        if (b.is_plugged) {
          bar.style.width = '100%';
          bar.style.background = 'var(--accent)';
        } else {
          bar.style.width = `${Math.min(100, Math.max(0, b.percent))}%`;
          bar.style.background = b.percent <= 20 ? 'var(--red)' : 'var(--green)';
        }
        
        const plugBadge = document.getElementById('bat-plugged');
        if (b.is_plugged) {
          plugBadge.innerText = '⚡ Plugged In';
          plugBadge.style.color = 'var(--accent)';
          plugBadge.style.borderColor = 'var(--accent)';
        } else {
          plugBadge.innerText = '🔋 On Battery';
          plugBadge.style.color = 'var(--green)';
          plugBadge.style.borderColor = 'var(--green)';
        }
      }

      if (data.wifi) {
        document.getElementById('wifi-ssid').innerText = data.wifi.ssid || '--';
        document.getElementById('wifi-ip').innerText = data.wifi.ip || '--';
        document.getElementById('wifi-rssi').innerText = `${data.wifi.rssi} dBm`;
      }

      if (data.system) {
        document.getElementById('sys-heap').innerText = `${(data.system.free_heap_bytes / 1024).toFixed(1)} KB`;
        document.getElementById('sys-min-heap').innerText = `${(data.system.min_free_heap_bytes / 1024).toFixed(1)} KB`;
        const s = data.system.uptime_seconds;
        const hrs = Math.floor(s / 3600);
        const mins = Math.floor((s % 3600) / 60);
        const secs = s % 60;
        document.getElementById('sys-uptime').innerText = `${hrs}h ${mins}m ${secs}s`;
      }

      if (data.time) {
        document.getElementById('time-formatted').innerText = data.time.formatted || '--';
        document.getElementById('time-epoch').innerText = data.time.epoch_seconds || '--';
      }
    }

    // Initial HTTP fetch
    fetch('/api/status').then(r => r.json()).then(updateUI).catch(console.error);

    // WebSocket connection for real-time live push updates
    function connectWs() {
      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
      const ws = new WebSocket(`${proto}//${location.host}/ws`);
      const badge = document.getElementById('ws-badge');
      const text = document.getElementById('ws-text');

      ws.onopen = () => {
        badge.className = 'badge badge-live';
        text.innerText = 'LIVE (WebSocket)';
      };

      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          updateUI(data);
        } catch (e) {
          console.error('WS parse error:', e);
        }
      };

      ws.onclose = () => {
        badge.className = 'badge badge-connecting';
        text.innerText = 'Reconnecting...';
        setTimeout(connectWs, 2000);
      };

      ws.onerror = () => ws.close();
    }

    connectWs();
  </script>
</body>
</html>
)rawliteral";

// A string as a JSON string, quotes included, escaped.
struct JsonString {
  std::string_view text;

  void format_to(coprocessor::Writer& out) const {
    out.write("\"");
    std::size_t start = 0;  // First character not written yet.
    for (std::size_t i = 0; i < text.size(); ++i) {
      const auto c = static_cast<unsigned char>(text[i]);
      if (c != '"' && c != '\\' && c >= 0x20) {
        continue;
      }
      out.write(text.substr(start, i - start));
      if (c == '"' || c == '\\') {
        const std::array<char, 2> escaped{'\\', static_cast<char>(c)};
        out.write({escaped.data(), escaped.size()});
      } else {
        constexpr std::string_view kHex = "0123456789abcdef";
        const std::array<char, 6> escaped{'\\', 'u',          '0',
                                          '0',  kHex[c >> 4], kHex[c & 0xf]};
        out.write({escaped.data(), escaped.size()});
      }
      start = i + 1;
    }
    out.write(text.substr(start));
    out.write("\"");
  }
};

constexpr std::string_view json_bool(bool value) {
  return value ? "true" : "false";
}

std::string build_status_json() {
  const bool connected = coprocessor::network_is_up();
  const std::string ssid = coprocessor::wifi_get_ssid();
  int8_t rssi = 0;
  wifi_ap_record_t ap_info{};
  if (connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }

  time_t now = 0;
  time(&now);
  tm utc{};
  gmtime_r(&now, &utc);
  std::array<char, 32> formatted{};
  const std::size_t formatted_size = strftime(
      formatted.data(), formatted.size(), "%Y-%m-%d %H:%M:%S UTC", &utc);

  std::array<char, 512> buffer{};
  coprocessor::Writer out(buffer);
  out.print(R"({{"battery":{{"millivolts":{},"percent":{},"is_plugged":{}}},)"
            R"("wifi":{{"connected":{},"ssid":{},"ip":"{}","rssi":{}}},)"
            R"("system":{{"uptime_seconds":{},"free_heap_bytes":{},)"
            R"("min_free_heap_bytes":{}}},)"
            R"("time":{{"epoch_seconds":{},"formatted":"{}"}}}})",
            g_battery_millivolts.load(), g_battery_percent.load(),
            json_bool(g_battery_plugged.load()), json_bool(connected),
            JsonString{ssid}, coprocessor::network_ip(), rssi,
            static_cast<uint32_t>(esp_timer_get_time() / 1'000'000),
            esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
            static_cast<int64_t>(now),
            std::string_view(formatted.data(), formatted_size));
  if (out.size() > buffer.size()) {
    ESP_LOGE(TAG, "Status JSON needs %u bytes",
             static_cast<unsigned>(out.size()));
    return "{}";
  }
  return {buffer.data(), out.size()};
}

struct WsBroadcastData {
  httpd_handle_t server;  // The one whose task runs the work: alive meanwhile.
  std::string json;
};

// The server's task.
void ws_broadcast_work(void* arg) {
  std::unique_ptr<WsBroadcastData> data(static_cast<WsBroadcastData*>(arg));
  httpd_ws_frame_t ws_frame{};
  ws_frame.type = HTTPD_WS_TYPE_TEXT;
  ws_frame.payload = reinterpret_cast<uint8_t*>(data->json.data());
  ws_frame.len = data->json.size();

  std::array<int, kMaxOpenSockets> client_fds{};
  size_t client_count = client_fds.size();
  if (httpd_get_client_list(data->server, &client_count, client_fds.data()) !=
      ESP_OK) {
    return;
  }
  for (size_t i = 0; i < client_count; ++i) {
    if (httpd_ws_get_fd_info(data->server, client_fds[i]) ==
        HTTPD_WS_CLIENT_WEBSOCKET) {
      httpd_ws_send_frame_async(data->server, client_fds[i], &ws_frame);
    }
  }
}

// Any task.
void broadcast_status() {
  std::scoped_lock lock(g_server_mutex);
  if (g_server == nullptr) {
    return;
  }
  auto data = std::make_unique<WsBroadcastData>(
      WsBroadcastData{.server = g_server, .json = build_status_json()});
  if (httpd_queue_work(g_server, ws_broadcast_work, data.get()) == ESP_OK) {
    data.release();  // ws_broadcast_work() owns it now.
  }
}

esp_err_t index_get_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML.data(), INDEX_HTML.size());
}

esp_err_t status_get_handler(httpd_req_t* req) {
  std::string json = build_status_json();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.data(), json.size());
}

#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
esp_err_t ws_post_handshake_cb(httpd_req_t* req) {
  ESP_LOGI(TAG,
           "New WebSocket client connected (fd=%d), sending initial status",
           httpd_req_to_sockfd(req));
  std::string json = build_status_json();
  httpd_ws_frame_t ws_pkt = {};
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  ws_pkt.payload = reinterpret_cast<uint8_t*>(json.data());
  ws_pkt.len = json.size();
  return httpd_ws_send_frame(req, &ws_pkt);
}
#endif

esp_err_t ws_handler(httpd_req_t* req) {
  httpd_ws_frame_t ws_pkt = {};
  std::array<uint8_t, 64> buffer = {};
  ws_pkt.payload = buffer.data();
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, buffer.size() - 1);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "httpd_ws_recv_frame failed: %d", ret);
    return ret;
  }
  return ESP_OK;
}

// GET /api/log?tag=heap&level=debug: sets a log tag's level at runtime, e.g.
// to turn on heap logging (heap_log.h) without reflashing. Levels: none,
// error, warn, info, debug, verbose.
esp_err_t log_level_handler(httpd_req_t* req) {
  std::array<char, 96> query = {};
  std::array<char, 32> tag = {};
  std::array<char, 16> level = {};
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK ||
      httpd_query_key_value(query.data(), "tag", tag.data(), tag.size()) !=
          ESP_OK ||
      httpd_query_key_value(query.data(), "level", level.data(),
                            level.size()) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "usage: /api/log?tag=<tag>&level=<level>");
  }
  constexpr std::array<std::pair<std::string_view, esp_log_level_t>, 6> kLevels{
      {{"none", ESP_LOG_NONE},
       {"error", ESP_LOG_ERROR},
       {"warn", ESP_LOG_WARN},
       {"info", ESP_LOG_INFO},
       {"debug", ESP_LOG_DEBUG},
       {"verbose", ESP_LOG_VERBOSE}}};
  const auto* it =
      std::ranges::find(kLevels, std::string_view(level.data()),
                        &std::pair<std::string_view, esp_log_level_t>::first);
  if (it == kLevels.end()) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown level");
  }
  esp_log_level_set(tag.data(), it->second);  // Copies the tag.
  ESP_LOGI(TAG, "Log level of '%s' set to %s", tag.data(), level.data());
  return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

// Spam guard state for the GENA NOTIFY endpoint (rate detection only, no
// throttling).
uint32_t g_notify_window_start_ms = 0;
uint32_t g_notify_count = 0;
bool g_notify_warned = false;

// Handles UPnP GENA NOTIFY requests from subscribed Sonos speakers: their
// listener parses the body inline, on this task, as it streams in.
esp_err_t media_notify_handler(httpd_req_t* req) {
  auto now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  if (now_ms - g_notify_window_start_ms >= 1000) {
    g_notify_window_start_ms = now_ms;
    g_notify_count = 0;
    g_notify_warned = false;
  }
  g_notify_count++;
  if (g_notify_count > 10 && !g_notify_warned) {
    g_notify_warned = true;
    ESP_LOGW(TAG, "NOTIFY flood detected: >10 requests/second");
  }

  std::array<char, 160> sid = {};
  if (httpd_req_get_hdr_value_str(req, "SID", sid.data(), sid.size()) !=
      ESP_OK) {
    sid[0] = '\0';
  }

  std::array<char, 48> where = {};
  std::snprintf(where.data(), where.size(), "NOTIFY received (%d B)",
                req->content_len);
  coprocessor::log_heap(where.data());

  // The listener parses the body inline, as it streams in, under the
  // registry's lock: its handler can't go away meanwhile.
  bool received = true;
  bool well_formed = true;
  const bool known =
      g_gena->dispatch(sid.data(), [&](coprocessor::NotifyListener& listener) {
        std::array<char, 512> buf = {};
        int remaining = req->content_len;
        bool first = true;
        if (remaining == 0) {
          well_formed = listener.on_notify({}, true, true);
          return;
        }
        while (remaining > 0 && well_formed) {
          const int n =
              httpd_req_recv(req, buf.data(),
                             std::min(remaining, static_cast<int>(buf.size())));
          if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
          }
          if (n <= 0) {
            received = false;
            return;
          }
          remaining -= n;
          well_formed = listener.on_notify(
              {buf.data(), static_cast<std::size_t>(n)}, first, remaining == 0);
          first = false;
        }
      });

  coprocessor::log_heap("NOTIFY handled");

  if (!known) {
    // An old or foreign subscription: 412 tells the speaker to drop it.
    ESP_LOGW(TAG, "NOTIFY for unknown SID '%s': 412", sid.data());
    httpd_resp_set_status(req, "412 Precondition Failed");
    return httpd_resp_send(req, "unknown sid", HTTPD_RESP_USE_STRLEN);
  }
  if (!received) {
    ESP_LOGW(TAG, "NOTIFY body recv failed");
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "recv failed");
  }
  if (!well_formed) {
    ESP_LOGW(TAG, "NOTIFY XML parse error");
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "xml parse error");
  }
  return httpd_resp_send(req, nullptr, 0);
}

}  // namespace

namespace coprocessor {

namespace {

esp_err_t web_server_start() {
  std::scoped_lock lock(g_server_mutex);
  if (g_server) {
    ESP_LOGI(TAG, "Web server already running");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 8;
  config.max_open_sockets = kMaxOpenSockets;
  // NOTIFYs are parsed inline, on this task. Measured in QEMU at -Og: 4 KB
  // used at most, by a LastChange with track metadata (a nested parse).
  config.stack_size = 8192;

  ESP_LOGI(TAG, "Starting web server on port %d...", config.server_port);
  esp_err_t ret = httpd_start(&g_server, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server: %d", ret);
    return ret;
  }

  httpd_uri_t index_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = index_get_handler,
      .user_ctx = nullptr,
      .is_websocket = false,
  };
  httpd_register_uri_handler(g_server, &index_uri);

  httpd_uri_t status_uri = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = status_get_handler,
      .user_ctx = nullptr,
      .is_websocket = false,
  };
  httpd_register_uri_handler(g_server, &status_uri);

  httpd_uri_t log_uri = {
      .uri = "/api/log",
      .method = HTTP_GET,
      .handler = log_level_handler,
      .user_ctx = nullptr,
      .is_websocket = false,
  };
  httpd_register_uri_handler(g_server, &log_uri);

  httpd_uri_t ws_uri = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = ws_handler,
      .user_ctx = nullptr,
      .is_websocket = true,
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
      .ws_post_handshake_cb = ws_post_handshake_cb,
#endif
  };
  httpd_register_uri_handler(g_server, &ws_uri);

  httpd_uri_t notify_uri = {
      .uri = "/media/notify",
      .method = HTTP_NOTIFY,
      .handler = media_notify_handler,
      .user_ctx = nullptr,
      .is_websocket = false,
  };
  httpd_register_uri_handler(g_server, &notify_uri);

  ESP_LOGI(TAG,
           "Web server running: /, /api/status, /api/log, /ws (WebSocket), "
           "/media/notify (GENA)");
  return ESP_OK;
}

esp_err_t web_server_stop() {
  std::scoped_lock lock(g_server_mutex);
  if (!g_server) {
    return ESP_OK;
  }
  ESP_LOGI(TAG, "Stopping web server...");
  esp_err_t ret = httpd_stop(g_server);
  g_server = nullptr;
  return ret;
}

}  // namespace

void web_server_update_battery(const CoprocessorProto::BatteryStatus* status) {
  if (!status) {
    return;
  }
  g_battery_millivolts.store(status->millivolts());
  bool plugged = status->is_plugged();
  g_battery_plugged.store(plugged);
  if (plugged || status->percent() == 0xFF) {
    g_battery_percent.store(-1);
  } else {
    g_battery_percent.store(static_cast<int32_t>(status->percent()));
  }

  broadcast_status();
}

namespace {

// Serves while there's an IP, on whichever interface has it.
void on_ip_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t id,
                 void* /*data*/) {
  switch (id) {
    case IP_EVENT_STA_GOT_IP:
    case IP_EVENT_ETH_GOT_IP:
      web_server_start();
      break;
    case IP_EVENT_STA_LOST_IP:
    case IP_EVENT_ETH_LOST_IP:
      web_server_stop();
      break;
    default:
      break;
  }
}

}  // namespace

void web_server_init(GenaRegistry& gena) {
  g_gena = &gena;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, nullptr, nullptr));
}

}  // namespace coprocessor
