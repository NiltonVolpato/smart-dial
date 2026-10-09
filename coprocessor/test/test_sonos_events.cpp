// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "net/ipv4.h"
#include "sonos/events.h"

namespace coprocessor {
namespace {

using CoprocessorProto::TransportState;

std::string fixture(const std::string& name) {
  std::ifstream file(std::string(SONOS_FIXTURES_DIR) + "/" + name);
  EXPECT_TRUE(file.is_open()) << name;
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

// Feeds `body` in small chunks, as a socket would deliver it.
template <class Parser>
bool feed_in_chunks(Parser& parser, std::string_view body,
                    std::size_t chunk = 97) {
  for (std::size_t at = 0; at < body.size(); at += chunk) {
    std::string_view piece = body.substr(at, chunk);
    if (!parser.feed(piece, at + piece.size() == body.size())) {
      return false;
    }
  }
  return true;
}

Ipv4 ip(std::string_view dotted) { return *Ipv4::parse(dotted); }

TEST(SonosEventsTest, TopologyNotify) {
  TopologyParser parser;
  ASSERT_TRUE(
      feed_in_chunks(parser, fixture("notify_zone_group_topology.xml")));
  std::vector<CoprocessorProto::GroupT> groups = parser.take();
  ASSERT_EQ(groups.size(), 4U);

  EXPECT_EQ(groups[0].name, "Office");
  EXPECT_EQ(groups[0].id, "RINCON_00000000000101400:499712146");
  EXPECT_EQ(Ipv4{groups[0].coordinator_ip}, ip("192.168.1.190"));
  ASSERT_EQ(groups[0].members.size(), 3U);
  EXPECT_EQ(groups[0].members[1]->name, "Dining Room");
  EXPECT_EQ(Ipv4{groups[0].members[1]->ip}, ip("192.168.1.168"));

  // Satellites are nested in their member, not members themselves.
  EXPECT_EQ(groups[1].name, "Living Room");
  EXPECT_EQ(Ipv4{groups[1].coordinator_ip}, ip("192.168.1.124"));
  EXPECT_EQ(groups[1].members.size(), 1U);

  // The second of a stereo pair is invisible.
  EXPECT_EQ(groups[2].name, "Bedroom Bed");
  EXPECT_EQ(groups[2].members.size(), 1U);

  EXPECT_EQ(groups[3].name, "Bathroom");
  EXPECT_EQ(Ipv4{groups[3].coordinator_ip}, ip("192.168.1.155"));
}

TEST(SonosEventsTest, TopologyNotifyInOneChunk) {
  TopologyParser parser;
  std::string body = fixture("notify_zone_group_topology.xml");
  ASSERT_TRUE(parser.feed(body, true));
  EXPECT_EQ(parser.take().size(), 4U);
}

TEST(SonosEventsTest, AvTransportPlaying) {
  LastChangeParser parser;
  ASSERT_TRUE(feed_in_chunks(parser, fixture("notify_avtransport_playing.xml")));
  CoprocessorProto::StateUpdateT state = parser.take();
  EXPECT_EQ(state.transport_state, TransportState::Playing);
  ASSERT_NE(state.track, nullptr);
  EXPECT_EQ(state.track->title, "The Rope (feat. POLIÇA)");
  EXPECT_EQ(state.track->artist, "Lane 8");
  EXPECT_EQ(state.track->album, "Brightest Lights");
  EXPECT_EQ(state.track->duration_seconds, 295U);
  EXPECT_FALSE(state.volume.has_value());  // Not AVTransport's to say.
}

TEST(SonosEventsTest, AvTransportPausedAndTransitioning) {
  LastChangeParser paused;
  ASSERT_TRUE(feed_in_chunks(paused, fixture("notify_avtransport_paused.xml")));
  EXPECT_EQ(paused.take().transport_state, TransportState::Paused);

  LastChangeParser transitioning;
  ASSERT_TRUE(feed_in_chunks(
      transitioning, fixture("notify_avtransport_transitioning.xml")));
  EXPECT_EQ(transitioning.take().transport_state,
            TransportState::Transitioning);
}

TEST(SonosEventsTest, RenderingControlMasterVolume) {
  LastChangeParser parser;
  ASSERT_TRUE(feed_in_chunks(parser, fixture("notify_rendering_control.xml")));
  CoprocessorProto::StateUpdateT state = parser.take();
  EXPECT_EQ(state.volume, 4);  // Master, not LF/RF.
  EXPECT_EQ(state.is_muted, false);
  EXPECT_FALSE(state.transport_state.has_value());
  EXPECT_EQ(state.track, nullptr);
}

TEST(SonosEventsTest, NotImplementedTrackMetadataIsSkipped) {
  // Line-in sources say NOT_IMPLEMENTED: it isn't XML, and must not fail the
  // rest of the event.
  constexpr std::string_view kBody =
      R"(<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property>)"
      R"(<LastChange>&lt;Event&gt;&lt;InstanceID val="0"&gt;)"
      R"(&lt;TransportState val="PLAYING"/&gt;)"
      R"(&lt;CurrentTrackMetaData val="NOT_IMPLEMENTED"/&gt;)"
      R"(&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>)"
      R"(</e:property></e:propertyset>)";
  LastChangeParser parser;
  ASSERT_TRUE(parser.feed(kBody, true));
  CoprocessorProto::StateUpdateT state = parser.take();
  EXPECT_EQ(state.transport_state, TransportState::Playing);
  EXPECT_EQ(state.track, nullptr);
}

TEST(SonosEventsTest, PositionInfo) {
  PositionParser parser;
  ASSERT_TRUE(
      feed_in_chunks(parser, fixture("get_position_info_playing.xml"), 31));
  EXPECT_EQ(parser.elapsed_seconds(), 0U);
  EXPECT_EQ(parser.duration_seconds(), 295U);
}

TEST(SonosEventsTest, ParseHms) {
  EXPECT_EQ(parse_hms("0:04:55"), 295U);
  EXPECT_EQ(parse_hms("1:00:00"), 3600U);
  EXPECT_EQ(parse_hms("NOT_IMPLEMENTED"), std::nullopt);
  EXPECT_EQ(parse_hms("0:61:00"), std::nullopt);
  EXPECT_EQ(parse_hms(""), std::nullopt);
}

TEST(SonosEventsTest, MalformedBodyFails) {
  TopologyParser parser;
  EXPECT_FALSE(parser.feed("<e:propertyset><unclosed>", true));
}

}  // namespace
}  // namespace coprocessor
