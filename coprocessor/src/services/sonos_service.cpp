// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/sonos_service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "esp_log.h"

#include "net/http_client.h"
#include "net/http_request.h"
#include "net/ipv4.h"
#include "services/rpc.h"
#include "services/sonos_streams.h"
#include "services/stream_handler.h"
#include "services/unary_handler.h"
#include "sonos/events.h"
#include "sonos/soap.h"
#include "xml_query/xml_query.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "sonos_svc";

using CoprocessorProto::ActionType;
using CoprocessorProto::AlbumArtChunk;
using CoprocessorProto::Empty;
using CoprocessorProto::GroupSummary;
using CoprocessorProto::TransportState;
using std::chrono::milliseconds;
using tether::CallError;
using tether::WireStatus;

WireStatus to_wire_status(const HttpResult& result) {
  switch (result.error) {
    case HttpError::None:
      // A UPnP fault (e.g. 701 "transition not available") comes as 500.
      if (result.status == 500) {
        return WireStatus::FAILED_PRECONDITION;
      }
      return result.status == 404 ? WireStatus::NOT_FOUND : WireStatus::UNKNOWN;
    case HttpError::BadAddress:
      return WireStatus::INVALID_ARGUMENT;
    case HttpError::Connect:
    case HttpError::Io:
      return WireStatus::UNAVAILABLE;
    case HttpError::Timeout:
      return WireStatus::DEADLINE_EXCEEDED;
    case HttpError::Protocol:
    case HttpError::Aborted:
      return WireStatus::UNKNOWN;
  }
  return WireStatus::UNKNOWN;
}

// Sends one SOAP command and answers the call with how it went.
class SoapCommandHandler : public UnaryHandler<Empty> {
 protected:
  SoapCommandHandler(Reactor& reactor, ConnectionPermits& permits,
                     tether::Reply<Empty> reply, Endpoint endpoint)
      : UnaryHandler(reactor, reply),
        endpoint_(endpoint),
        http_(reactor, permits) {}

  template <SelfFormatting Args>
  void execute(const SoapService& service, std::string_view action,
               const Args& args) {
    action_ = action;
    request_ = soap_request(endpoint_, service, action, args);
    if (!request_.fits()) {
      ESP_LOGE(TAG, "%.*s request too large", static_cast<int>(action.size()),
               action.data());
      fail_and_finish(WireStatus::INTERNAL);
      return;
    }
    http_.send(endpoint_, request_.bytes(),
               tether::Callback(this, &SoapCommandHandler::executed));
  }

  void executed() {
    const HttpResult& result = http_.result();
    ESP_LOGI(TAG, "%.*s on %s:%u -> HTTP %d (error %d)",
             static_cast<int>(action_.size()), action_.data(),
             endpoint_.ip.to_string().c_str(), endpoint_.port, result.status,
             static_cast<int>(result.error));
    if (result.ok()) {
      reply_and_finish([](flatbuffers::FlatBufferBuilder& fbb) {
        return CoprocessorProto::CreateEmpty(fbb);
      });
    } else {
      fail_and_finish(to_wire_status(result));
    }
  }

  Endpoint endpoint_;
  HttpClient http_;
  SoapRequest request_;
  std::string_view action_;  // A literal: for logs.
};

// Appends the text a query produces to a string.
struct AppendText {
  std::string* out;
  void operator()(std::string_view chunk) const { out->append(chunk); }
};

// CurrentTransportState's text, in a GetTransportInfo response.
constexpr auto kTransportState =
    xq::Select("CurrentTransportState") | xq::Text();
using TransportStateParser =
    xq::XmlParser<std::remove_const_t<decltype(kTransportState)>, AppendText>;

class ActionHandler final : public SoapCommandHandler, HttpResponseSink {
 public:
  // Tether I/O task: copies the request out of its frame.
  ActionHandler(Reactor& reactor, ConnectionPermits& permits,
                tether::Reply<Empty> reply,
                const CoprocessorProto::ActionRequest& request)
      : SoapCommandHandler(
            reactor, permits, reply,
            {.ip = Ipv4{request.endpoint_ip()}, .port = kSonosPort}),
        action_type_(request.action()) {}

 private:
  void start() override {
    if (action_type_ == ActionType::TogglePlayPause) {
      // Toggling needs the current state first.
      request_ = soap_request(endpoint_, kAvTransport, "GetTransportInfo",
                              InstanceArgs{});
      parser_.emplace(kTransportState, AppendText{&state_});
      http_.send(endpoint_, request_.bytes(),
                 tether::Callback(this, &ActionHandler::state_known), this);
    } else {
      run(action_type_);
    }
  }

  // HttpResponseSink: the GetTransportInfo body.
  bool on_body(std::string_view chunk, bool last) override {
    return parser_->feed(chunk, last) == xq::ParseStatus::Ok;
  }

  void state_known() {
    std::optional<TransportState> state;
    if (http_.result().ok()) {
      state = parse_transport_state(state_.c_str());
    }
    parser_.reset();
    if (!state) {
      ESP_LOGW(TAG, "GetTransportInfo failed, toggling to Play");
    }
    run(state == TransportState::Playing ? ActionType::Pause
                                         : ActionType::Play);
  }

  void run(ActionType action) {
    switch (action) {
      case ActionType::Play:
        execute(kAvTransport, "Play", PlayArgs{});
        return;
      case ActionType::Pause:
        execute(kAvTransport, "Pause", InstanceArgs{});
        return;
      case ActionType::Next:
        execute(kAvTransport, "Next", InstanceArgs{});
        return;
      case ActionType::Previous:
        execute(kAvTransport, "Previous", InstanceArgs{});
        return;
      case ActionType::Stop:
        execute(kAvTransport, "Stop", InstanceArgs{});
        return;
      case ActionType::TogglePlayPause:
        break;  // Resolved in start().
    }
    fail_and_finish(WireStatus::INVALID_ARGUMENT);
  }

  ActionType action_type_;
  std::string state_;
  std::optional<TransportStateParser> parser_;
};

class SetVolumeHandler final : public SoapCommandHandler {
 public:
  // Tether I/O task: copies the request out of its frame.
  SetVolumeHandler(Reactor& reactor, ConnectionPermits& permits,
                   tether::Reply<Empty> reply,
                   const CoprocessorProto::VolumeRequest& request)
      : SoapCommandHandler(
            reactor, permits, reply,
            {.ip = Ipv4{request.endpoint_ip()}, .port = kSonosPort}),
        volume_(request.volume()),
        is_relative_(request.is_relative()) {}

 private:
  void start() override {
    if (is_relative_) {
      execute(kRenderingControl, "SetRelativeVolume",
              SetRelativeVolumeArgs{.adjustment = volume_});
    } else {
      execute(kRenderingControl, "SetVolume",
              SetVolumeArgs{.volume = static_cast<uint8_t>(
                                std::clamp<int16_t>(volume_, 0, 100))});
    }
  }

  int16_t volume_;
  bool is_relative_;
};

// The title in a GetPositionInfo response: the DIDL-Lite document nested, as
// text, in TrackMetaData.
constexpr auto kTrackTitle = xq::Select("TrackMetaData") | xq::Text() |
                             xq::ParseXml() | xq::Select("title") | xq::Text();
using TrackTitleParser =
    xq::XmlParser<std::remove_const_t<decltype(kTrackTitle)>, AppendText>;

// Cuts `text` to at most `size` bytes, on a UTF-8 character boundary.
void truncate_utf8(std::string& text, std::size_t size) {
  if (text.size() <= size) {
    return;
  }
  while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) {
    --size;  // A continuation byte: back off to its character's start.
  }
  text.resize(size);
}

// Summarizes groups by coordinator, one at a time, sending each as soon as
// it's known: the client shows them as they come. A group whose speaker
// fails to answer is still sent, as not playing. Ends after the last.
class GroupsSummaryHandler final : public StreamHandler<GroupSummary> {
 public:
  // Tether I/O task: copies the request out of its frame.
  GroupsSummaryHandler(Reactor& reactor, ConnectionPermits& permits,
                       tether::Sink<GroupSummary> sink,
                       const CoprocessorProto::GroupsSummaryRequest& request)
      : StreamHandler(reactor, sink), query_(reactor, permits, *this) {
    if (const auto* ips = request.coordinator_ips()) {
      coordinators_.reserve(ips->size());
      for (uint32_t ip : *ips) {
        coordinators_.push_back(Ipv4{ip});
      }
    }
  }

 private:
  // Bounds an item's size.
  static constexpr std::size_t kMaxTitle = 48;

  struct Group {
    Ipv4 coordinator;
    bool is_playing = false;
    std::string title;
  };

  // One query slot: GetTransportInfo, then GetPositionInfo if playing.
  class Query final : public HttpResponseSink {
   public:
    Query(Reactor& reactor, ConnectionPermits& permits,
          GroupsSummaryHandler& handler)
        : handler_(handler), http_(reactor, permits) {}

    void start(Group& group) {
      group_ = &group;
      state_.clear();
      send("GetTransportInfo", kTransportState, &state_, &Query::state_known);
    }

   private:
    // Sends `action`, appending the text `query` finds in the response to
    // `out`, then calls `next`.
    template <class XmlQuery>
    void send(std::string_view action, const XmlQuery& query, std::string* out,
              void (Query::*next)()) {
      endpoint_ = {.ip = group_->coordinator, .port = kSonosPort};
      request_ = soap_request(endpoint_, kAvTransport, action, InstanceArgs{});
      parser_.emplace<xq::XmlParser<XmlQuery, AppendText>>(query,
                                                           AppendText{out});
      http_.send(endpoint_, request_.bytes(), tether::Callback(this, next),
                 this);
    }

    bool on_body(std::string_view chunk, bool last) override {
      return std::visit(
          [&](auto& parser) {
            if constexpr (std::is_same_v<decltype(parser), std::monostate&>) {
              return false;
            } else {
              return parser.feed(chunk, last) == xq::ParseStatus::Ok;
            }
          },
          parser_);
    }

    void state_known() {
      const HttpResult& result = http_.result();
      if (!result.ok()) {
        ESP_LOGW(TAG, "Summary: GetTransportInfo on %s -> HTTP %d (error %d)",
                 group_->coordinator.to_string().c_str(), result.status,
                 static_cast<int>(result.error));
      }
      group_->is_playing =
          result.ok() &&
          parse_transport_state(state_.c_str()) == TransportState::Playing;
      parser_ = std::monostate{};
      if (!group_->is_playing) {
        handler_.query_done();  // Last: it may delete us all.
        return;
      }
      send("GetPositionInfo", kTrackTitle, &group_->title, &Query::title_known);
    }

    void title_known() {
      parser_ = std::monostate{};
      if (http_.result().ok()) {
        truncate_utf8(group_->title, kMaxTitle);
      } else {
        group_->title.clear();  // A failed parse may have left part of it.
      }
      handler_.query_done();  // Last: it may delete us all.
    }

    GroupsSummaryHandler& handler_;
    HttpClient http_;
    Endpoint endpoint_;
    SoapRequest request_;
    Group* group_ = nullptr;
    std::string state_;
    std::variant<std::monostate, TransportStateParser, TrackTitleParser>
        parser_;
  };

  void start() override { next(); }

  // Queries the next coordinator, or ends the stream after the last.
  void next() {
    if (closed()) {
      finish();  // The client cancelled.
      return;
    }
    if (next_ == coordinators_.size()) {
      end_and_finish(WireStatus::OK);
      return;
    }
    group_ = Group{.coordinator = coordinators_[next_++]};
    query_.start(group_);
  }

  void query_done() { send_group(); }

  void ready() override { send_group(); }

  void send_group() {
    auto result = send([&](flatbuffers::FlatBufferBuilder& fbb) {
      return CoprocessorProto::CreateGroupSummaryDirect(
          fbb, group_.coordinator.value, group_.is_playing,
          group_.title.c_str());
    });
    if (result) {
      next();
      return;
    }
    switch (result.error()) {
      case CallError::QueueFull:
      case CallError::NoCredit:
        return;  // ready() sends it again.
      case CallError::Closed:
        finish();
        return;
      case CallError::TooLarge:
        ESP_LOGE(TAG, "Summary of %s doesn't fit a message: skipped",
                 group_.coordinator.to_string().c_str());
        next();
        return;
    }
  }

  Query query_;
  std::vector<Ipv4> coordinators_;  // Summarized in request order.
  std::size_t next_ = 0;            // The next coordinator to query.
  Group group_;                     // The one being queried or sent.
};

// Streams an image on the speaker as it downloads, in chunks of at most
// Limits.MaxAlbumArtChunk bytes; the first carries the image's size, if
// known. While the stream has no room the download waits, so a chunk at most
// is held here whatever the image's size.
class AlbumArtHandler final : public StreamHandler<AlbumArtChunk>,
                              HttpResponseSink {
 public:
  // Tether I/O task: builds the request out of the call's frame.
  AlbumArtHandler(Reactor& reactor, ConnectionPermits& permits,
                  tether::Sink<AlbumArtChunk> sink,
                  const CoprocessorProto::AlbumArtRequest& request)
      : StreamHandler(reactor, sink),
        http_(reactor, permits),
        endpoint_{.ip = Ipv4{request.endpoint_ip()}, .port = kSonosPort} {
    const std::string_view uri =
        request.uri() != nullptr ? request.uri()->string_view() : "";
    valid_ = uri.starts_with('/');  // Only paths on the speaker, for now.
    request_.append(R"(GET {} HTTP/1.1
HOST: {}:{}
Connection: close

)",
                    uri, endpoint_.ip, endpoint_.port);
  }

 private:
  static constexpr std::size_t kMaxChunk =
      static_cast<std::size_t>(CoprocessorProto::Limits::MaxAlbumArtChunk);
  // Fits the speaker's /getaa paths, which carry the track's URI.
  using ArtRequest = FixedRequest<640>;

  void start() override {
    if (!valid_ || !request_.fits()) {
      end_and_finish(WireStatus::INVALID_ARGUMENT);
      return;
    }
    started_ = Reactor::Clock::now();
    http_.send(endpoint_, request_.bytes(),
               tether::Callback(this, &AlbumArtHandler::downloaded), this);
  }

  // One line per download: what it took, and how often the stream had no
  // room (QueueFull: the link; NoCredit: the client).
  void log_outcome(const char* outcome) const {
    const auto elapsed = std::chrono::duration_cast<milliseconds>(
        Reactor::Clock::now() - started_);
    ESP_LOGI(
        TAG,
        "Album art %s: %u B in %u chunks, %u ms; retries: %u queue full, "
        "%u no credit; longest pause %u ms",
        outcome, static_cast<unsigned>(received_),
        static_cast<unsigned>(chunks_), static_cast<unsigned>(elapsed.count()),
        static_cast<unsigned>(queue_full_), static_cast<unsigned>(no_credit_),
        static_cast<unsigned>(longest_pause_.count()));
  }

  // Returning false aborts the download: the client is gone.
  bool on_body(std::string_view chunk, bool /*last*/) override {
    std::ranges::copy(chunk, staging_.begin() + staged_);
    staged_ += chunk.size();
    received_ += chunk.size();
    return send_ready();
  }

  void downloaded() {
    const HttpResult& result = http_.result();
    if (gone_) {
      log_outcome("cancelled");
      finish();
    } else if (!result.ok()) {
      ESP_LOGW(TAG, "Album art from %s -> HTTP %d (error %d)",
               endpoint_.ip.to_string().c_str(), result.status,
               static_cast<int>(result.error));
      log_outcome("failed");
      end_and_finish(to_wire_status(result));
    } else {
      download_done_ = true;
      after_send(send_ready());
    }
  }

  void ready() override { after_send(send_ready()); }

  // Outside the download's callbacks: the stream may end here.
  void after_send(bool sendable) {
    if (!sendable) {
      finish();
    } else if (download_done_ && staged_ == 0) {
      log_outcome("sent");
      end_and_finish(WireStatus::OK);
    }
  }

  // Sends whole chunks, and the rest once the download is done. Pauses the
  // download while the stream has no room, and resumes it once there's room
  // again for another read. False once the stream is gone.
  bool send_ready() {
    while (staged_ >= kMaxChunk || (download_done_ && staged_ > 0)) {
      const std::size_t size = std::min(staged_, kMaxChunk);
      auto result = send([&](flatbuffers::FlatBufferBuilder& fbb) {
        auto data = fbb.CreateVector(
            reinterpret_cast<const uint8_t*>(staging_.data()), size);
        const auto total =
            first_ ? static_cast<uint32_t>(http_.content_length().value_or(0))
                   : 0U;
        return CoprocessorProto::CreateAlbumArtChunk(fbb, total, data);
      });
      if (!result) {
        if (no_room(result.error())) {
          ++(result.error() == CallError::QueueFull ? queue_full_ : no_credit_);
          if (!paused_since_) {
            paused_since_ = Reactor::Clock::now();
          }
          http_.pause();  // Until ready().
          return true;
        }
        gone_ = true;  // Closed, or a chunk that can't fit: a bug.
        return false;
      }
      first_ = false;
      ++chunks_;
      std::copy(staging_.begin() + size, staging_.begin() + staged_,
                staging_.begin());
      staged_ -= size;
    }
    if (paused_since_) {
      longest_pause_ =
          std::max(longest_pause_, std::chrono::duration_cast<milliseconds>(
                                       Reactor::Clock::now() - *paused_since_));
      paused_since_.reset();
    }
    http_.resume();
    return true;
  }

  HttpClient http_;
  Endpoint endpoint_;
  ArtRequest request_;
  bool valid_ = false;
  // A chunk short of full, and what one more read brings.
  std::array<char, kMaxChunk + HttpClient::kBufferSize> staging_{};
  std::size_t staged_ = 0;
  bool first_ = true;
  bool download_done_ = false;
  bool gone_ = false;  // The client cancelled, while downloading.
  // For log_outcome().
  Reactor::Clock::time_point started_;
  std::optional<Reactor::Clock::time_point> paused_since_;
  milliseconds longest_pause_{0};
  std::size_t received_ = 0;
  std::size_t chunks_ = 0;
  std::size_t queue_full_ = 0;
  std::size_t no_credit_ = 0;
};

}  // namespace

void SonosServiceHandler::watch_topology(
    tether::Sink<CoprocessorProto::TopologyUpdate> sink,
    const CoprocessorProto::TopologyRequest& request) {
  ESP_LOGI(TAG, "Watch topology (seed %s)",
           Ipv4{request.seed_ip()}.to_string().c_str());
  reactor_.start(
      new_watch_topology(reactor_, permits_, registry_, sink, request));
}

void SonosServiceHandler::watch_playback(
    tether::Sink<CoprocessorProto::StateUpdate> sink,
    const CoprocessorProto::PlaybackRequest& request) {
  ESP_LOGI(TAG, "Watch playback (coordinator %s)",
           Ipv4{request.endpoint_ip()}.to_string().c_str());
  reactor_.start(
      new_watch_playback(reactor_, permits_, registry_, sink, request));
}

void SonosServiceHandler::action(
    tether::Reply<CoprocessorProto::Empty> reply,
    const CoprocessorProto::ActionRequest& request) {
  ESP_LOGI(TAG, "Sonos action %s",
           CoprocessorProto::EnumNameActionType(request.action()));
  reactor_.start(new ActionHandler(reactor_, permits_, reply, request));
}

void SonosServiceHandler::set_volume(
    tether::Reply<CoprocessorProto::Empty> reply,
    const CoprocessorProto::VolumeRequest& request) {
  ESP_LOGI(TAG, "Sonos volume %d (relative=%d)", request.volume(),
           request.is_relative());
  reactor_.start(new SetVolumeHandler(reactor_, permits_, reply, request));
}

void SonosServiceHandler::get_album_art(
    tether::Sink<CoprocessorProto::AlbumArtChunk> sink,
    const CoprocessorProto::AlbumArtRequest& request) {
  ESP_LOGI(TAG, "Album art from %s",
           Ipv4{request.endpoint_ip()}.to_string().c_str());
  reactor_.start(new AlbumArtHandler(reactor_, permits_, sink, request));
}

void SonosServiceHandler::get_groups_summary(
    tether::Sink<CoprocessorProto::GroupSummary> sink,
    const CoprocessorProto::GroupsSummaryRequest& request) {
  reactor_.start(new GroupsSummaryHandler(reactor_, permits_, sink, request));
}

}  // namespace coprocessor
