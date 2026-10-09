// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/sonos_streams.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_log.h"

#include "net/http_client.h"
#include "net/ipv4.h"
#include "services/stream_handler.h"
#include "sonos/events.h"
#include "sonos/soap.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "sonos_streams";

using CoprocessorProto::GroupT;
using CoprocessorProto::StateUpdate;
using CoprocessorProto::StateUpdateT;
using CoprocessorProto::TopologyUpdate;
using std::chrono::milliseconds;
using std::chrono::seconds;
using tether::CallError;
using tether::WireStatus;

// How long to wait before sending again, when the stream had no room.
constexpr milliseconds kSendRetry{50};
// How long to wait before renewing again, after a failed renewal.
constexpr seconds kRenewRetry{30};

// A send that didn't go through: whether to retry (no room), or stop.
bool should_retry(CallError error) {
  return error == CallError::QueueFull || error == CallError::NoCredit;
}

// --- Topology --------------------------------------------------------------

// Subscribes to ZoneGroupTopology on the seed speaker. Each NOTIFY carries
// the whole household (the first one right after subscribing), sent as one
// item per group under a new sequence number.
class WatchTopologyHandler final
    : public NotifiedStreamHandler<TopologyUpdate> {
 public:
  WatchTopologyHandler(Reactor& reactor, ConnectionPermits& permits,
                       GenaRegistry& registry,
                       tether::Sink<TopologyUpdate> sink, Ipv4 seed)
      : NotifiedStreamHandler(reactor, registry, sink),
        subscription_(reactor, permits, registry, *this,
                      {.ip = seed, .port = kSonosPort},
                      "/ZoneGroupTopology/Event") {}

  ~WatchTopologyHandler() override {
    reactor().cancel_timer(renew_timer_);
    reactor().cancel_timer(send_timer_);
  }

 private:
  void start() override {
    subscription_.refresh(Callback(this, &WatchTopologyHandler::refreshed));
  }

  void refreshed() {
    if (!subscription_.subscribed()) {
      if (!ever_subscribed_) {
        stop(WireStatus::UNAVAILABLE);  // The seed doesn't answer.
        return;
      }
      renew_timer_ = reactor().after(
          kRenewRetry, Callback(this, &WatchTopologyHandler::renew));
      return;
    }
    ever_subscribed_ = true;
    renew_timer_ =
        reactor().after(subscription_.renew_after(),
                        Callback(this, &WatchTopologyHandler::renew));
  }

  void renew() {
    renew_timer_ = 0;
    if (closed()) {
      stop(std::nullopt);  // Its destructor unsubscribes.
      return;
    }
    subscription_.refresh(Callback(this, &WatchTopologyHandler::refreshed));
  }

  // Web server task.
  bool on_notify(std::string_view chunk, bool first, bool last) override {
    if (first) {
      parser_.emplace();
    }
    if (!parser_->feed(chunk, last)) {
      return false;
    }
    if (last) {
      {
        std::scoped_lock lock(mutex_);
        pending_ = parser_->take();
        has_pending_ = true;
      }
      request_flush();
    }
    return true;
  }

  void flush() override {
    {
      std::scoped_lock lock(mutex_);
      if (!has_pending_) {
        return;
      }
      sending_ = std::move(pending_);
      has_pending_ = false;
    }
    // A newer household replaces one still being sent.
    reactor().cancel_timer(send_timer_);
    ++sequence_;
    next_ = 0;
    send_groups();
  }

  void send_groups() {
    send_timer_ = 0;
    const auto total = static_cast<uint16_t>(sending_.size());
    if (total == 0) {
      send_one(nullptr, 0, 0);  // No groups: still news.
      return;
    }
    while (next_ < total) {
      if (!send_one(&sending_[next_], next_, total)) {
        return;
      }
      ++next_;
    }
  }

  // Returns false if it stopped, or must retry later.
  bool send_one(const GroupT* group, uint16_t index, uint16_t total) {
    auto result = send([&](flatbuffers::FlatBufferBuilder& fbb) {
      auto group_offset = group != nullptr
                              ? CoprocessorProto::CreateGroup(fbb, group)
                              : flatbuffers::Offset<CoprocessorProto::Group>{};
      return CoprocessorProto::CreateTopologyUpdate(fbb, sequence_, index,
                                                    total, group_offset);
    });
    if (result) {
      return true;
    }
    if (should_retry(result.error())) {
      send_timer_ = reactor().after(
          kSendRetry, Callback(this, &WatchTopologyHandler::send_groups));
    } else if (result.error() == CallError::Closed) {
      stop(std::nullopt);
    } else {
      ESP_LOGE(TAG, "Topology group %u doesn't fit a message: skipped",
               static_cast<unsigned>(index));
      return true;
    }
    return false;
  }

  GenaSubscription subscription_;
  Reactor::TimerId renew_timer_ = 0;
  Reactor::TimerId send_timer_ = 0;
  bool ever_subscribed_ = false;

  std::optional<TopologyParser> parser_;  // Web server task only.

  std::mutex mutex_;  // Between the web server's task and the reactor.
  std::vector<GroupT> pending_;
  bool has_pending_ = false;

  std::vector<GroupT> sending_;  // Reactor only, from here on.
  uint32_t sequence_ = 0;
  uint16_t next_ = 0;
};

// --- Playback --------------------------------------------------------------

// Fields set in `from` overwrite `into`'s.
void merge(StateUpdateT& into, StateUpdateT&& from) {
  if (from.transport_state) {
    into.transport_state = from.transport_state;
  }
  if (from.volume) {
    into.volume = from.volume;
  }
  if (from.is_muted) {
    into.is_muted = from.is_muted;
  }
  if (from.track) {
    into.track = std::move(from.track);
  }
}

// Subscribes to AVTransport (state, track) and RenderingControl (volume,
// mute) on the group's coordinator. When the state or track changes, it also
// asks GetPositionInfo for the elapsed time, which events don't carry.
class WatchPlaybackHandler final : public NotifiedStreamHandler<StateUpdate>,
                                   public HttpResponseSink {
 public:
  WatchPlaybackHandler(Reactor& reactor, ConnectionPermits& permits,
                       GenaRegistry& registry, tether::Sink<StateUpdate> sink,
                       Ipv4 coordinator)
      : NotifiedStreamHandler(reactor, registry, sink),
        coordinator_{.ip = coordinator, .port = kSonosPort},
        transport_(reactor, permits, registry, *this, coordinator_,
                   "/MediaRenderer/AVTransport/Event"),
        rendering_(reactor, permits, registry, *this, coordinator_,
                   "/MediaRenderer/RenderingControl/Event"),
        position_http_(reactor, permits) {}

  ~WatchPlaybackHandler() override {
    reactor().cancel_timer(renew_timer_);
    reactor().cancel_timer(send_timer_);
  }

 private:
  void start() override { renew(); }

  // Both subscriptions, one after the other.
  void renew() {
    renew_timer_ = 0;
    if (closed()) {
      stop(std::nullopt);  // The subscriptions' destructors unsubscribe.
      return;
    }
    transport_.refresh(Callback(this, &WatchPlaybackHandler::transport_done));
  }

  void transport_done() {
    if (!transport_.subscribed() && !ever_subscribed_) {
      stop(WireStatus::UNAVAILABLE);  // The coordinator doesn't answer.
      return;
    }
    rendering_.refresh(Callback(this, &WatchPlaybackHandler::rendering_done));
  }

  void rendering_done() {
    const bool both = transport_.subscribed() && rendering_.subscribed();
    ever_subscribed_ = ever_subscribed_ || transport_.subscribed();
    renew_timer_ = reactor().after(
        both ? std::min(transport_.renew_after(), rendering_.renew_after())
             : kRenewRetry,
        Callback(this, &WatchPlaybackHandler::renew));
  }

  // Web server task: AVTransport's and RenderingControl's NOTIFYs alike.
  bool on_notify(std::string_view chunk, bool first, bool last) override {
    if (first) {
      parser_.emplace();
    }
    if (!parser_->feed(chunk, last)) {
      return false;
    }
    if (last) {
      {
        std::scoped_lock lock(mutex_);
        merge(pending_, parser_->take());
        has_pending_ = true;
      }
      request_flush();
    }
    return true;
  }

  void flush() override {
    StateUpdateT news;
    {
      std::scoped_lock lock(mutex_);
      if (!has_pending_) {
        return;
      }
      news = std::exchange(pending_, {});
      has_pending_ = false;
    }
    const bool moved = news.transport_state.has_value() || news.track;
    merge(outgoing_, std::move(news));
    has_outgoing_ = true;
    if (moved && !querying_position_) {
      query_position();
    } else if (!querying_position_) {
      send_state();
    }
    // Else: the position query, when done, sends everything merged so far.
  }

  void query_position() {
    querying_position_ = true;
    position_parser_.emplace();
    position_request_ = soap_request(coordinator_, kAvTransport,
                                     "GetPositionInfo", InstanceArgs{});
    position_http_.send(coordinator_, position_request_.bytes(),
                        Callback(this, &WatchPlaybackHandler::position_known),
                        this);
  }

  // The GetPositionInfo body.
  bool on_body(std::string_view chunk, bool last) override {
    return position_parser_->feed(chunk, last);
  }

  void position_known() {
    querying_position_ = false;
    if (position_http_.result().ok()) {
      if (!outgoing_.track) {
        outgoing_.track = std::make_unique<CoprocessorProto::TrackMetadataT>();
      }
      if (auto elapsed = position_parser_->elapsed_seconds()) {
        outgoing_.track->elapsed_seconds = *elapsed;
      }
      if (auto duration = position_parser_->duration_seconds();
          duration && !outgoing_.track->duration_seconds) {
        outgoing_.track->duration_seconds = *duration;
      }
    }
    send_state();
  }

  void send_state() {
    send_timer_ = 0;
    if (!has_outgoing_) {
      return;
    }
    outgoing_.endpoint_ip = coordinator_.ip.value;
    auto result = send([&](flatbuffers::FlatBufferBuilder& fbb) {
      return CoprocessorProto::CreateStateUpdate(fbb, &outgoing_);
    });
    if (result) {
      outgoing_ = {};
      has_outgoing_ = false;
    } else if (should_retry(result.error())) {
      send_timer_ = reactor().after(
          kSendRetry, Callback(this, &WatchPlaybackHandler::send_state));
    } else if (result.error() == CallError::Closed) {
      stop(std::nullopt);
    } else {
      ESP_LOGE(TAG, "State update doesn't fit a message: dropped");
      outgoing_ = {};
      has_outgoing_ = false;
    }
  }

  Endpoint coordinator_;
  GenaSubscription transport_;
  GenaSubscription rendering_;
  Reactor::TimerId renew_timer_ = 0;
  Reactor::TimerId send_timer_ = 0;
  bool ever_subscribed_ = false;

  std::optional<LastChangeParser> parser_;  // Web server task only.

  std::mutex mutex_;  // Between the web server's task and the reactor.
  StateUpdateT pending_;
  bool has_pending_ = false;

  // Reactor only, from here on.
  StateUpdateT outgoing_;
  bool has_outgoing_ = false;
  HttpClient position_http_;
  SoapRequest position_request_;
  std::optional<PositionParser> position_parser_;
  bool querying_position_ = false;
};

}  // namespace

BaseHandler* new_watch_topology(
    Reactor& reactor, ConnectionPermits& permits, GenaRegistry& registry,
    tether::Sink<CoprocessorProto::TopologyUpdate> sink,
    const CoprocessorProto::TopologyRequest& request) {
  return new WatchTopologyHandler(reactor, permits, registry, sink,
                                  Ipv4{request.seed_ip()});
}

BaseHandler* new_watch_playback(
    Reactor& reactor, ConnectionPermits& permits, GenaRegistry& registry,
    tether::Sink<CoprocessorProto::StateUpdate> sink,
    const CoprocessorProto::PlaybackRequest& request) {
  return new WatchPlaybackHandler(reactor, permits, registry, sink,
                                  Ipv4{request.endpoint_ip()});
}

}  // namespace coprocessor
