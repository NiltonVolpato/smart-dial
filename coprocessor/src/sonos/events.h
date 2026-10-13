// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// Parsers for what speakers send: GENA NOTIFY bodies and SOAP responses, as
// they stream in, into the messages the S3 receives.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "coprocessor_generated.h"

namespace coprocessor {

/// "PLAYING", "PAUSED_PLAYBACK", "STOPPED", "TRANSITIONING".
std::optional<CoprocessorProto::TransportState> parse_transport_state(
    std::string_view text);

/// "H:MM:SS" into seconds; nullopt for anything else (e.g. NOT_IMPLEMENTED).
std::optional<uint32_t> parse_hms(std::string_view text);

/// A ZoneGroupTopology NOTIFY: its ZoneGroupState, as groups of visible
/// members, named after (and addressed at) their coordinator.
class TopologyParser {
 public:
  TopologyParser();
  ~TopologyParser();
  TopologyParser(const TopologyParser&) = delete;
  TopologyParser& operator=(const TopologyParser&) = delete;

  /// False if the body is malformed: stop feeding then.
  bool feed(std::string_view chunk, bool last);
  /// The groups, once the last chunk was fed; none if the NOTIFY carried no
  /// ZoneGroupState (only other variables, e.g. an alarm's), as opposed to
  /// an empty one.
  std::optional<std::vector<CoprocessorProto::GroupT>> take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// An AVTransport or RenderingControl NOTIFY: the fields its LastChange
/// carries (transport state and track, or volume and mute); the rest stay
/// unset.
class LastChangeParser {
 public:
  LastChangeParser();
  ~LastChangeParser();
  LastChangeParser(const LastChangeParser&) = delete;
  LastChangeParser& operator=(const LastChangeParser&) = delete;

  bool feed(std::string_view chunk, bool last);
  CoprocessorProto::StateUpdateT take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// A GetPositionInfo response: elapsed time and duration.
class PositionParser {
 public:
  PositionParser();
  ~PositionParser();
  PositionParser(const PositionParser&) = delete;
  PositionParser& operator=(const PositionParser&) = delete;

  bool feed(std::string_view chunk, bool last);
  [[nodiscard]] std::optional<uint32_t> elapsed_seconds() const;
  [[nodiscard]] std::optional<uint32_t> duration_seconds() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace coprocessor
