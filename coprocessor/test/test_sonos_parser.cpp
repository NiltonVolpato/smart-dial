// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <stddef.h>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "coprocessor_generated.h"
#include "sonos_parser.h"
#include "sonos_serialization.h"

namespace coprocessor {
namespace {

std::string load_fixture(const std::string& filename) {
  std::string path = std::string(SONOS_FIXTURES_DIR) + "/" + filename;
  std::ifstream file(path);
  if (!file.is_open()) {
    ADD_FAILURE() << "Failed to open fixture: " << path;
    return "";
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

TEST(SonosParserTest, ParseHms) {
  EXPECT_EQ(parse_hms("0:00:00"), 0u);
  EXPECT_EQ(parse_hms("0:04:55"), 295u);
  EXPECT_EQ(parse_hms("1:02:03"), 3723u);
  EXPECT_EQ(parse_hms("NOT_IMPLEMENTED"), std::nullopt);
  EXPECT_EQ(parse_hms("not_implemented"), std::nullopt);
  EXPECT_EQ(parse_hms("-0:01:00"), std::nullopt);
  EXPECT_EQ(parse_hms(""), std::nullopt);
  EXPECT_EQ(parse_hms(nullptr), std::nullopt);
  EXPECT_EQ(parse_hms("invalid"), std::nullopt);
}

TEST(SonosParserTest, ParseTransportState) {
  EXPECT_EQ(parse_transport_state("PLAYING"),
            CoprocessorProto::TransportState::Playing);
  EXPECT_EQ(parse_transport_state("PAUSED_PLAYBACK"),
            CoprocessorProto::TransportState::Paused);
  EXPECT_EQ(parse_transport_state("STOPPED"),
            CoprocessorProto::TransportState::Stopped);
  EXPECT_EQ(parse_transport_state("TRANSITIONING"),
            CoprocessorProto::TransportState::Transitioning);
  EXPECT_EQ(parse_transport_state("UNKNOWN"), std::nullopt);
  EXPECT_EQ(parse_transport_state(nullptr), std::nullopt);
}

TEST(SonosParserTest, GetPositionInfoPlaying) {
  std::string xml = load_fixture("get_position_info_playing.xml");
  ASSERT_FALSE(xml.empty());

  SonosTrackMetadata meta;
  ASSERT_TRUE(parse_position_info(xml, &meta));
  EXPECT_EQ(meta.title, "The Rope (feat. POLIÇA)");
  EXPECT_EQ(meta.artist, "Lane 8");
  EXPECT_EQ(meta.album, "Brightest Lights");
  EXPECT_EQ(meta.duration_seconds, 295u);
  EXPECT_EQ(meta.elapsed_seconds, 0u);

  // FlatBuffer Serialization & Verification
  SonosStateUpdate state;
  state.endpoint_ip = "192.168.1.100";
  state.transport_state = CoprocessorProto::TransportState::Playing;
  state.track = meta;

  flatbuffers::FlatBufferBuilder fbb;
  auto offset = serialize_state_update(fbb, state);
  fbb.Finish(offset);

  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  ASSERT_TRUE(verifier.VerifyBuffer<CoprocessorProto::StateUpdate>(nullptr));

  const auto* update =
      flatbuffers::GetRoot<CoprocessorProto::StateUpdate>(fbb.GetBufferPointer());
  ASSERT_NE(update, nullptr);
  EXPECT_STREQ(update->endpoint_ip()->c_str(), "192.168.1.100");
  EXPECT_EQ(update->transport_state().value(),
            CoprocessorProto::TransportState::Playing);

  const auto* track = update->track();
  ASSERT_NE(track, nullptr);
  EXPECT_STREQ(track->title()->c_str(), "The Rope (feat. POLIÇA)");
  EXPECT_STREQ(track->artist()->c_str(), "Lane 8");
  EXPECT_STREQ(track->album()->c_str(), "Brightest Lights");
  EXPECT_EQ(track->duration_seconds().value(), 295u);
  EXPECT_EQ(track->elapsed_seconds().value(), 0u);
}

TEST(SonosParserTest, GetPositionInfoPaused) {
  std::string xml = load_fixture("get_position_info_paused.xml");
  ASSERT_FALSE(xml.empty());

  SonosTrackMetadata meta;
  ASSERT_TRUE(parse_position_info(xml, &meta));
  EXPECT_EQ(meta.title, "The Rope (feat. POLIÇA)");
  EXPECT_EQ(meta.artist, "Lane 8");
  EXPECT_EQ(meta.album, "Brightest Lights");
  EXPECT_EQ(meta.duration_seconds, 295u);
  EXPECT_EQ(meta.elapsed_seconds, 0u);
}

TEST(SonosParserTest, GetPositionInfoStopped) {
  std::string xml = load_fixture("get_position_info_stopped.xml");
  ASSERT_FALSE(xml.empty());

  SonosTrackMetadata meta;
  ASSERT_TRUE(parse_position_info(xml, &meta));
  EXPECT_EQ(meta.title, std::nullopt);
  EXPECT_EQ(meta.artist, std::nullopt);
  EXPECT_EQ(meta.album, std::nullopt);
  EXPECT_EQ(meta.duration_seconds, 0u);
  EXPECT_EQ(meta.elapsed_seconds, 0u);
}

TEST(SonosParserTest, GetTransportInfo) {
  std::string playing_xml = load_fixture("get_transport_info_playing.xml");
  ASSERT_FALSE(playing_xml.empty());
  CoprocessorProto::TransportState state_playing;
  ASSERT_TRUE(parse_transport_info(playing_xml, &state_playing));
  EXPECT_EQ(state_playing, CoprocessorProto::TransportState::Playing);

  std::string paused_xml = load_fixture("get_transport_info_paused.xml");
  ASSERT_FALSE(paused_xml.empty());
  CoprocessorProto::TransportState state_paused;
  ASSERT_TRUE(parse_transport_info(paused_xml, &state_paused));
  EXPECT_EQ(state_paused, CoprocessorProto::TransportState::Paused);

  std::string stopped_xml = load_fixture("get_transport_info_stopped.xml");
  ASSERT_FALSE(stopped_xml.empty());
  CoprocessorProto::TransportState state_stopped;
  ASSERT_TRUE(parse_transport_info(stopped_xml, &state_stopped));
  EXPECT_EQ(state_stopped, CoprocessorProto::TransportState::Stopped);
}

TEST(SonosParserTest, GetVolume) {
  std::string xml = load_fixture("get_volume.xml");
  ASSERT_FALSE(xml.empty());

  uint8_t volume = 0;
  ASSERT_TRUE(parse_volume_info(xml, &volume));
  EXPECT_EQ(volume, 4);
}

TEST(SonosParserTest, GetZoneGroupStateTopology) {
  std::string xml = load_fixture("get_zone_group_state.xml");
  ASSERT_FALSE(xml.empty());

  std::vector<SonosGroup> groups;
  ASSERT_TRUE(parse_zone_group_state(xml, &groups));
  ASSERT_EQ(groups.size(), 4u);

  // Group 0: Bedroom Bed
  EXPECT_EQ(groups[0].name, "Bedroom Bed");
  EXPECT_EQ(groups[0].coordinator_ip, "192.168.1.208");
  EXPECT_EQ(groups[0].coordinator_port, 1400);

  // Group 1: Living Room
  EXPECT_EQ(groups[1].name, "Living Room");
  EXPECT_EQ(groups[1].coordinator_ip, "192.168.1.124");
  EXPECT_EQ(groups[1].coordinator_port, 1400);
  // Invisible satellites should be filtered out
  EXPECT_EQ(groups[1].members.size(), 1u);

  // Group 2: Office (Coordinator + Kitchen + Dining Room)
  EXPECT_EQ(groups[2].name, "Office");
  EXPECT_EQ(groups[2].coordinator_ip, "192.168.1.190");
  EXPECT_EQ(groups[2].coordinator_port, 1400);
  EXPECT_EQ(groups[2].members.size(), 3u);

  // Group 3: Bathroom
  EXPECT_EQ(groups[3].name, "Bathroom");
  EXPECT_EQ(groups[3].coordinator_ip, "192.168.1.155");
  EXPECT_EQ(groups[3].coordinator_port, 1400);

  // FlatBuffer Topology Serialization & Verification (Office group)
  flatbuffers::FlatBufferBuilder fbb;
  auto offset = serialize_topology_update(fbb, 1, 2, groups.size(), groups[2]);
  fbb.Finish(offset);

  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  ASSERT_TRUE(verifier.VerifyBuffer<CoprocessorProto::TopologyUpdate>(nullptr));

  const auto* topo =
      flatbuffers::GetRoot<CoprocessorProto::TopologyUpdate>(fbb.GetBufferPointer());
  ASSERT_NE(topo, nullptr);
  EXPECT_EQ(topo->sequence(), 1u);
  EXPECT_EQ(topo->group_index(), 2u);
  EXPECT_EQ(topo->total_groups(), 4u);

  const auto* grp = topo->group();
  ASSERT_NE(grp, nullptr);
  EXPECT_STREQ(grp->name()->c_str(), "Office");
  EXPECT_STREQ(grp->coordinator_ip()->c_str(), "192.168.1.190");
  EXPECT_EQ(grp->members()->size(), 3u);
}

TEST(SonosParserTest, NotifyAVTransportPlaying) {
  std::string xml = load_fixture("notify_avtransport_playing.xml");
  ASSERT_FALSE(xml.empty());

  NotifyResult notify;
  ASSERT_TRUE(parse_notify_body(xml, &notify));
  EXPECT_FALSE(notify.zone_group_state);
  ASSERT_EQ(notify.last_changes.size(), 1u);

  LastChangeResult lc;
  ASSERT_TRUE(parse_last_change(notify.last_changes[0], &lc));
  EXPECT_EQ(lc.transport, CoprocessorProto::TransportState::Playing);
  ASSERT_TRUE(lc.track.has_value());
  EXPECT_EQ(lc.track->title, "The Rope (feat. POLIÇA)");
  EXPECT_EQ(lc.track->artist, "Lane 8");
  EXPECT_EQ(lc.track->album, "Brightest Lights");
  EXPECT_EQ(lc.track->duration_seconds, 295u);
}

TEST(SonosParserTest, NotifyAVTransportTransitioning) {
  std::string xml = load_fixture("notify_avtransport_transitioning.xml");
  ASSERT_FALSE(xml.empty());

  NotifyResult notify;
  ASSERT_TRUE(parse_notify_body(xml, &notify));
  ASSERT_EQ(notify.last_changes.size(), 1u);

  LastChangeResult lc;
  ASSERT_TRUE(parse_last_change(notify.last_changes[0], &lc));
  EXPECT_EQ(lc.transport, CoprocessorProto::TransportState::Transitioning);
}

TEST(SonosParserTest, NotifyAVTransportPaused) {
  std::string xml = load_fixture("notify_avtransport_paused.xml");
  ASSERT_FALSE(xml.empty());

  NotifyResult notify;
  ASSERT_TRUE(parse_notify_body(xml, &notify));
  ASSERT_EQ(notify.last_changes.size(), 1u);

  LastChangeResult lc;
  ASSERT_TRUE(parse_last_change(notify.last_changes[0], &lc));
  EXPECT_EQ(lc.transport, CoprocessorProto::TransportState::Paused);
}

TEST(SonosParserTest, NotifyRenderingControl) {
  std::string xml = load_fixture("notify_rendering_control.xml");
  ASSERT_FALSE(xml.empty());

  NotifyResult notify;
  ASSERT_TRUE(parse_notify_body(xml, &notify));
  ASSERT_EQ(notify.last_changes.size(), 1u);

  LastChangeResult lc;
  ASSERT_TRUE(parse_last_change(notify.last_changes[0], &lc));
  EXPECT_EQ(lc.volume, 4);
  EXPECT_EQ(lc.muted, false);
}

TEST(SonosParserTest, StreamingNotifyParser18KBPayload) {
  std::string xml = load_fixture("notify_zone_group_topology.xml");
  ASSERT_FALSE(xml.empty());
  ASSERT_EQ(xml.size(), 18911u);

  StreamingNotifyParser parser;
  // Feed in tiny 64-byte chunks to test streaming behavior
  constexpr size_t kChunkSize = 64;
  for (size_t offset = 0; offset < xml.size(); offset += kChunkSize) {
    size_t chunk = std::min(kChunkSize, xml.size() - offset);
    bool is_final = (offset + chunk >= xml.size());
    ASSERT_TRUE(parser.feed_chunk(xml.data() + offset, chunk, is_final));
  }

  NotifyResult result;
  ASSERT_TRUE(parser.finish(&result));
  EXPECT_TRUE(result.zone_group_state);
  EXPECT_TRUE(result.last_changes.empty());
}

TEST(SonosParserTest, ErrorHandling) {
  SonosTrackMetadata meta;
  EXPECT_FALSE(parse_position_info("", &meta));
  EXPECT_FALSE(parse_position_info("<malformed><unclosed>", &meta));

  CoprocessorProto::TransportState ts;
  EXPECT_FALSE(parse_transport_info("<empty/>", &ts));

  uint8_t vol = 0;
  EXPECT_FALSE(parse_volume_info("<empty/>", &vol));

  std::vector<SonosGroup> groups;
  EXPECT_FALSE(parse_zone_group_state("<not_xml", &groups));

  NotifyResult notify;
  EXPECT_FALSE(parse_notify_body("<bad xml", &notify));
}

}  // namespace
}  // namespace coprocessor
