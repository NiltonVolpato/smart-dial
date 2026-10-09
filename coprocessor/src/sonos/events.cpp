// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "sonos/events.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/ipv4.h"
#include "xml_query/xml_query.h"

namespace coprocessor {

namespace {

using CoprocessorProto::GroupT;
using CoprocessorProto::MemberT;
using CoprocessorProto::StateUpdateT;
using CoprocessorProto::TrackMetadataT;
using CoprocessorProto::TransportState;

template <class... Fns>
struct Overloaded : Fns... {
  using Fns::operator()...;
};

template <class T>
std::optional<T> parse_number(std::string_view text) {
  T value{};
  const char* end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

// "http://192.168.1.190:1400/xml/device_description.xml" -> 192.168.1.190.
std::optional<Ipv4> host_of(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    return std::nullopt;
  }
  url.remove_prefix(scheme + 3);
  return Ipv4::parse(url.substr(0, url.find_first_of(":/")));
}

// --- Topology ----------------------------------------------------------------

struct GroupRow {
  std::string id;
  std::string coordinator;  // The coordinator's UUID.
};

struct MemberRow {
  std::string uuid;
  std::string name;
  std::string location;
  std::string invisible;  // "1" for satellites, stereo pairs' second, ...
};

// Members arrive before their group: Construct emits each at its end tag,
// and a group ends after its members.
struct TopologyRows {
  std::vector<MemberRow> members;  // Of the group being read.
  std::vector<GroupT> groups;

  void add_group(const GroupRow& row) {
    GroupT group;
    group.id = row.id;
    const MemberRow* coordinator = nullptr;
    for (const MemberRow& member : members) {
      const std::optional<Ipv4> ip = host_of(member.location);
      if (!ip) {
        continue;
      }
      if (member.uuid == row.coordinator) {
        coordinator = &member;
      }
      if (member.invisible != "1") {
        auto visible = std::make_unique<MemberT>();
        visible->name = member.name;
        visible->uuid = member.uuid;
        visible->ip = ip->value;
        group.members.push_back(std::move(visible));
      }
    }
    if (coordinator != nullptr) {
      group.name = coordinator->name;
      group.coordinator_ip = host_of(coordinator->location)->value;
    } else if (!group.members.empty()) {
      group.name = group.members.front()->name;
      group.coordinator_ip = group.members.front()->ip;
    }
    if (!group.members.empty()) {
      groups.push_back(std::move(group));
    }
    members.clear();
  }
};

constexpr auto kTopology =
    xq::Select("ZoneGroupState") | xq::Text() | xq::ParseXml() |
    xq::Fanout(
        xq::Select("ZoneGroup") |
            xq::Construct<GroupRow>(
                xq::Bind(&GroupRow::id, xq::Attr("ID")),
                xq::Bind(&GroupRow::coordinator, xq::Attr("Coordinator"))),
        xq::Select("ZoneGroupMember") |
            xq::Construct<MemberRow>(
                xq::Bind(&MemberRow::uuid, xq::Attr("UUID")),
                xq::Bind(&MemberRow::name, xq::Attr("ZoneName")),
                xq::Bind(&MemberRow::location, xq::Attr("Location")),
                xq::Bind(&MemberRow::invisible, xq::Attr("Invisible"))));

auto topology_parser(TopologyRows* rows) {
  return xq::XmlParser(
      kTopology,
      Overloaded{
          [rows](const GroupRow& row) { rows->add_group(row); },
          [rows](const MemberRow& row) { rows->members.push_back(row); },
      });
}

// --- LastChange
// ----------------------------------------------------------------

struct LastChangeRow {
  std::string transport;
  std::string volume;  // The Master channel's.
  std::string mute;    // The Master channel's.
  std::string duration;
  std::string title;
  std::string artist;
  std::string album;
  std::string album_art_uri;
  bool seen = false;
};

// An element of the DIDL-Lite document that CurrentTrackMetaData carries,
// escaped, in its val attribute. Skips an empty or NOT_IMPLEMENTED value: it
// isn't XML, and would fail the whole parse.
constexpr auto track_field(std::string_view element) {
  auto has_didl = xq::Predicate([](const xq::Attributes& attrs) {
    const std::optional<std::string_view> val = attrs.get("val");
    return val && !val->empty() && *val != "NOT_IMPLEMENTED";
  });
  return xq::First(xq::Select("CurrentTrackMetaData").with(has_didl) |
                   xq::Attr("val") | xq::ParseXml() | xq::Select(element) |
                   xq::Text());
}

constexpr auto kMaster = xq::Attr("channel", "Master");

constexpr auto kLastChange =
    xq::Select("LastChange") | xq::Text() | xq::ParseXml() |
    xq::Select("InstanceID") |
    xq::Construct<LastChangeRow>(
        xq::Bind(&LastChangeRow::transport,
                 xq::First(xq::Select("TransportState") | xq::Attr("val"))),
        xq::Bind(
            &LastChangeRow::volume,
            xq::First(xq::Select("Volume").with(kMaster) | xq::Attr("val"))),
        xq::Bind(&LastChangeRow::mute,
                 xq::First(xq::Select("Mute").with(kMaster) | xq::Attr("val"))),
        xq::Bind(
            &LastChangeRow::duration,
            xq::First(xq::Select("CurrentTrackDuration") | xq::Attr("val"))),
        xq::Bind(&LastChangeRow::title, track_field("title")),
        xq::Bind(&LastChangeRow::artist, track_field("creator")),
        xq::Bind(&LastChangeRow::album, track_field("album")),
        xq::Bind(&LastChangeRow::album_art_uri, track_field("albumArtURI")));

auto last_change_parser(LastChangeRow* row) {
  return xq::XmlParser(kLastChange, [row](const LastChangeRow& parsed) {
    *row = parsed;
    row->seen = true;
  });
}

// --- GetPositionInfo
// -----------------------------------------------------------

struct PositionRow {
  std::string elapsed;
  std::string duration;
};

constexpr auto kPosition =
    xq::Select("GetPositionInfoResponse") |
    xq::Construct<PositionRow>(
        xq::Bind(&PositionRow::elapsed,
                 xq::First(xq::Select("RelTime") | xq::Text())),
        xq::Bind(&PositionRow::duration,
                 xq::First(xq::Select("TrackDuration") | xq::Text())));

auto position_parser(PositionRow* row) {
  return xq::XmlParser(kPosition,
                       [row](const PositionRow& parsed) { *row = parsed; });
}

}  // namespace

std::optional<TransportState> parse_transport_state(std::string_view text) {
  if (text == "PLAYING") {
    return TransportState::Playing;
  }
  if (text == "PAUSED_PLAYBACK") {
    return TransportState::Paused;
  }
  if (text == "STOPPED") {
    return TransportState::Stopped;
  }
  if (text == "TRANSITIONING") {
    return TransportState::Transitioning;
  }
  return std::nullopt;
}

std::optional<uint32_t> parse_hms(std::string_view text) {
  const std::size_t first = text.find(':');
  const std::size_t second = text.rfind(':');
  if (first == std::string_view::npos || first == second) {
    return std::nullopt;
  }
  auto hours = parse_number<uint32_t>(text.substr(0, first));
  auto minutes =
      parse_number<uint32_t>(text.substr(first + 1, second - first - 1));
  auto seconds = parse_number<uint32_t>(text.substr(second + 1));
  if (!hours || !minutes || !seconds || *minutes >= 60 || *seconds >= 60) {
    return std::nullopt;
  }
  return *hours * 3600 + *minutes * 60 + *seconds;
}

struct TopologyParser::Impl {
  TopologyRows rows;
  decltype(topology_parser(nullptr)) parser = topology_parser(&rows);
};

TopologyParser::TopologyParser() : impl_(std::make_unique<Impl>()) {}
TopologyParser::~TopologyParser() = default;

bool TopologyParser::feed(std::string_view chunk, bool last) {
  return impl_->parser.feed(chunk, last) == xq::ParseStatus::Ok;
}

std::vector<GroupT> TopologyParser::take() {
  return std::exchange(impl_->rows.groups, {});
}

struct LastChangeParser::Impl {
  LastChangeRow row;
  decltype(last_change_parser(nullptr)) parser = last_change_parser(&row);
};

LastChangeParser::LastChangeParser() : impl_(std::make_unique<Impl>()) {}
LastChangeParser::~LastChangeParser() = default;

bool LastChangeParser::feed(std::string_view chunk, bool last) {
  return impl_->parser.feed(chunk, last) == xq::ParseStatus::Ok;
}

StateUpdateT LastChangeParser::take() {
  const LastChangeRow& row = impl_->row;
  StateUpdateT state;
  if (auto transport = parse_transport_state(row.transport)) {
    state.transport_state = *transport;
  }
  if (auto volume = parse_number<int>(row.volume)) {
    state.volume = static_cast<uint8_t>(std::clamp(*volume, 0, 100));
  }
  if (!row.mute.empty()) {
    state.is_muted = row.mute == "1" || row.mute == "true";
  }
  std::optional<uint32_t> duration = parse_hms(row.duration);
  if (!row.title.empty() || !row.artist.empty() || !row.album.empty() ||
      duration) {
    state.track = std::make_unique<TrackMetadataT>();
    state.track->title = row.title;
    state.track->artist = row.artist;
    state.track->album = row.album;
    state.track->album_art_uri = row.album_art_uri;
    if (duration) {
      state.track->duration_seconds = *duration;
    }
  }
  return state;
}

struct PositionParser::Impl {
  PositionRow row;
  decltype(position_parser(nullptr)) parser = position_parser(&row);
};

PositionParser::PositionParser() : impl_(std::make_unique<Impl>()) {}
PositionParser::~PositionParser() = default;

bool PositionParser::feed(std::string_view chunk, bool last) {
  return impl_->parser.feed(chunk, last) == xq::ParseStatus::Ok;
}

std::optional<uint32_t> PositionParser::elapsed_seconds() const {
  return parse_hms(impl_->row.elapsed);
}

std::optional<uint32_t> PositionParser::duration_seconds() const {
  return parse_hms(impl_->row.duration);
}

}  // namespace coprocessor
