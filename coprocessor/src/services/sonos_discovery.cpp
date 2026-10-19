// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/sonos_discovery.h"

#include <mdns.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "esp_log.h"

#include "net/ipv4.h"
#include "services/stream_handler.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "sonos_discovery";

using CoprocessorProto::Speaker;
using std::chrono::milliseconds;
using tether::CallError;
using tether::WireStatus;

constexpr std::string_view kService = "_sonos._tcp.local.";
// Non-zero: replies to anyone else's query are dropped.
constexpr uint16_t kQueryId = 0x50D5;
// Wi-Fi drops multicast now and then, so the query goes out three times: at
// 0, 250 ms and 1 s. Each waits this long for the next, the last until the
// stream ends at 2 s. Answers come within ~100 ms.
constexpr std::array<milliseconds, 3> kAfterQuery{
    milliseconds{250}, milliseconds{750}, milliseconds{1000}};
// Bounds memory if something floods us with answers.
constexpr std::size_t kMaxSpeakers = 64;

// Sends one query for kService, then each speaker that answers, once: its
// IPv4 from the A record. The advertised port (1443, TLS) isn't the one we
// use (1400), so SRV is ignored.
class DiscoverHandler final : public StreamHandler<Speaker> {
 public:
  DiscoverHandler(Reactor& reactor, bool& running, tether::Sink<Speaker> sink)
      : StreamHandler(reactor, sink), running_(running) {}

  ~DiscoverHandler() override {
    reactor().cancel_timer(timer_);
    if (fd_ >= 0) {
      reactor().forget(fd_);
      mdns_socket_close(fd_);
    }
    if (owner_) {
      running_ = false;
    }
  }

 private:
  void start() override {
    if (running_) {
      end_and_finish(WireStatus::RESOURCE_EXHAUSTED);
      return;
    }
    running_ = owner_ = true;
    // An ephemeral port: speakers answer by unicast, to it.
    fd_ = mdns_socket_open_ipv4(nullptr);
    if (fd_ < 0) {
      ESP_LOGW(TAG, "No mDNS socket (errno %d)", errno);
      end_and_finish(WireStatus::UNAVAILABLE);
      return;
    }
    reactor().when_readable(fd_,
                            tether::Callback(this, &DiscoverHandler::readable));
    query();
  }

  void query() {
    timer_ = 0;
    if (mdns_query_send(fd_, MDNS_RECORDTYPE_PTR, kService.data(),
                        kService.size(), buffer_.data(), buffer_.size(),
                        kQueryId) < 0) {
      ESP_LOGW(TAG, "mDNS query not sent (errno %d)", errno);
    } else {
      sent_any_ = true;
    }
    const bool last = ++queries_ == kAfterQuery.size();
    timer_ =
        reactor().after(kAfterQuery[queries_ - 1],
                        tether::Callback(this, last ? &DiscoverHandler::expire
                                                    : &DiscoverHandler::query));
  }

  // One datagram: mdns_query_recv() reads one, or none (EAGAIN).
  void readable() {
    mdns_query_recv(fd_, buffer_.data(), buffer_.size(), &on_record, this,
                    kQueryId);
    if (send_found()) {
      reactor().when_readable(
          fd_, tether::Callback(this, &DiscoverHandler::readable));
    }
  }

  static int on_record(int /*sock*/, const sockaddr* /*from*/,
                       std::size_t /*addrlen*/, mdns_entry_type_t /*entry*/,
                       uint16_t /*query_id*/, uint16_t rtype,
                       uint16_t /*rclass*/, uint32_t /*ttl*/, const void* data,
                       std::size_t size, std::size_t /*name_offset*/,
                       std::size_t /*name_length*/, std::size_t record_offset,
                       std::size_t record_length, void* user_data) {
    if (rtype == MDNS_RECORDTYPE_A) {
      sockaddr_in addr{};
      mdns_record_parse_a(data, size, record_offset, record_length, &addr);
      static_cast<DiscoverHandler*>(user_data)->found(
          Ipv4{ntohl(addr.sin_addr.s_addr)});
    }
    return 0;  // Go on with the next record.
  }

  void found(Ipv4 ip) {
    if (ip.value == 0 || speakers_.size() == kMaxSpeakers ||
        std::ranges::contains(speakers_, ip)) {
      return;
    }
    ESP_LOGI(TAG, "Speaker at %s", ip.to_string().c_str());
    speakers_.push_back(ip);
  }

  void expire() {
    timer_ = 0;
    expired_ = true;
    send_found();
  }

  void ready() override { send_found(); }

  void on_closed() override { finish(); }

  // Sends what's new, then ends if the time is up. False once this handler
  // is gone (touch nothing of it then).
  bool send_found() {
    while (sent_ < speakers_.size()) {
      const Ipv4 ip = speakers_[sent_];
      auto result = send([&](flatbuffers::FlatBufferBuilder& fbb) {
        return CoprocessorProto::CreateSpeaker(fbb, ip.value);
      });
      if (!result) {
        if (no_room(result.error())) {
          return true;  // ready() goes on.
        }
        if (result.error() == CallError::Closed) {
          finish();
          return false;
        }
        ESP_LOGE(TAG, "Speaker doesn't fit a message: skipped");
      }
      ++sent_;
    }
    if (expired_) {
      ESP_LOGI(TAG, "Discovery done: %u speaker(s)",
               static_cast<unsigned>(speakers_.size()));
      // Nothing ever went out: no network, most likely.
      end_and_finish(sent_any_ ? WireStatus::OK : WireStatus::UNAVAILABLE);
      return false;
    }
    return true;
  }

  bool& running_;       // Reactor only.
  bool owner_ = false;  // This one set running_.
  int fd_ = -1;
  Reactor::TimerId timer_ = 0;
  std::size_t queries_ = 0;
  bool sent_any_ = false;
  bool expired_ = false;
  std::vector<Ipv4> speakers_;  // Found, in order.
  std::size_t sent_ = 0;        // How many of speakers_ went out.
  // One datagram, sent or received. lwIP doesn't reassemble IP fragments
  // (CONFIG_LWIP_IP4_REASSEMBLY), so none is bigger than an Ethernet frame.
  std::array<std::byte, 1500> buffer_{};
};

}  // namespace

BaseHandler* new_discover(Reactor& reactor, bool& running,
                          tether::Sink<Speaker> sink) {
  return new DiscoverHandler(reactor, running, sink);
}

}  // namespace coprocessor
