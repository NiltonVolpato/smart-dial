// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "services/sonos_service.h"

#include <algorithm>
#include <array>
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
#include "net/ipv4.h"
#include "services/rpc.h"
#include "services/sonos_streams.h"
#include "services/unary_handler.h"
#include "sonos/events.h"
#include "sonos/soap.h"
#include "xml_query/xml_query.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "sonos_svc";

using CoprocessorProto::ActionType;
using CoprocessorProto::Empty;
using CoprocessorProto::GroupsSummary;
using CoprocessorProto::TransportState;
using tether::WireStatus;

WireStatus to_wire_status(const HttpResult& result) {
  switch (result.error) {
    case HttpError::None:
      // A UPnP fault (e.g. 701 "transition not available") comes as 500.
      return result.status == 500 ? WireStatus::FAILED_PRECONDITION
                                  : WireStatus::UNKNOWN;
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
               Callback(this, &SoapCommandHandler::executed));
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

// The parser of a GetTransportInfo response, appending CurrentTransportState's
// text to `out`.
auto transport_state_parser(std::string* out) {
  auto query =
      xq::Select("CurrentTransportState") | xq::Text() |
      xq::Invoke([out](std::string_view chunk) { out->append(chunk); });
  return xq::XmlParser(query(xq::NullSink{}));
}

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
      parser_.emplace(transport_state_parser(&state_));
      http_.send(endpoint_, request_.bytes(),
                 Callback(this, &ActionHandler::state_known), this);
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
  std::optional<decltype(transport_state_parser(nullptr))> parser_;
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
// text, in TrackMetaData. Appends it to `out`.
auto track_title_parser(std::string* out) {
  auto query =
      xq::Select("TrackMetaData") | xq::Text() | xq::ParseXml() |
      xq::Select("title") | xq::Text() |
      xq::Invoke([out](std::string_view chunk) { out->append(chunk); });
  return xq::XmlParser(query(xq::NullSink{}));
}

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

// Summarizes groups by coordinator, a few at a time: each query slot takes
// the next coordinator as it frees up. A group whose speaker fails to answer
// is still listed, as not playing.
class GroupsSummaryHandler final : public UnaryHandler<GroupsSummary> {
 public:
  // Tether I/O task: copies the request out of its frame.
  GroupsSummaryHandler(Reactor& reactor, ConnectionPermits& permits,
                       tether::Reply<GroupsSummary> reply,
                       const CoprocessorProto::GroupsSummaryRequest& request)
      : UnaryHandler(reactor, reply),
        queries_{{{reactor, permits, *this},
                  {reactor, permits, *this},
                  {reactor, permits, *this}}} {
    if (const auto* ips = request.coordinator_ips()) {
      groups_.reserve(ips->size());
      for (uint32_t ip : *ips) {
        groups_.push_back({.coordinator = Ipv4{ip}});
      }
    }
  }

 private:
  // Keeps the reply within tether's payload: ten groups fit comfortably.
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
      send("GetTransportInfo", transport_state_parser(&state_),
           &Query::state_known);
    }

   private:
    template <class Parser>
    void send(std::string_view action, Parser parser, void (Query::*next)()) {
      endpoint_ = {.ip = group_->coordinator, .port = kSonosPort};
      request_ = soap_request(endpoint_, kAvTransport, action, InstanceArgs{});
      parser_.emplace<Parser>(std::move(parser));
      http_.send(endpoint_, request_.bytes(), Callback(this, next), this);
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
      if (!group_->is_playing) {
        handler_.query_done(*this);  // Last: it may delete us all.
        return;
      }
      send("GetPositionInfo", track_title_parser(&group_->title),
           &Query::title_known);
    }

    void title_known() {
      if (http_.result().ok()) {
        truncate_utf8(group_->title, kMaxTitle);
      } else {
        group_->title.clear();  // A failed parse may have left part of it.
      }
      handler_.query_done(*this);  // Last: it may delete us all.
    }

    GroupsSummaryHandler& handler_;
    HttpClient http_;
    Endpoint endpoint_;
    SoapRequest request_;
    Group* group_ = nullptr;
    std::string state_;
    std::variant<std::monostate, decltype(transport_state_parser(nullptr)),
                 decltype(track_title_parser(nullptr))>
        parser_;
  };

  void start() override {
    for (Query& query : queries_) {
      if (next_ == groups_.size()) {
        break;
      }
      ++active_;
      query.start(groups_[next_++]);
    }
    if (active_ == 0) {
      reply();  // No groups.
    }
  }

  // A slot finished its group: give it the next one, or answer when all are.
  void query_done(Query& query) {
    if (next_ < groups_.size()) {
      query.start(groups_[next_++]);
    } else if (--active_ == 0) {
      reply();
    }
  }

  void reply() {
    reply_and_finish([&](flatbuffers::FlatBufferBuilder& fbb) {
      std::vector<flatbuffers::Offset<CoprocessorProto::GroupSummary>> offsets;
      offsets.reserve(groups_.size());
      for (const Group& group : groups_) {
        offsets.push_back(CoprocessorProto::CreateGroupSummaryDirect(
            fbb, group.coordinator.value, group.is_playing,
            group.title.c_str()));
      }
      return CoprocessorProto::CreateGroupsSummaryDirect(fbb, &offsets);
    });
  }

  std::array<Query, SonosServiceHandler::kMaxHttpClientsPerCall> queries_;
  std::vector<Group> groups_;  // Answered in request order.
  std::size_t next_ = 0;       // The next group to query.
  std::size_t active_ = 0;     // Slots with a query in flight.
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

void SonosServiceHandler::get_groups_summary(
    tether::Reply<CoprocessorProto::GroupsSummary> reply,
    const CoprocessorProto::GroupsSummaryRequest& request) {
  reactor_.start(new GroupsSummaryHandler(reactor_, permits_, reply, request));
}

}  // namespace coprocessor
