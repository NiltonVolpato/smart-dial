// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

#include "tether/typed.h"

#include "async/reactor.h"
#include "coprocessor_rpc.h"
#include "net/connection_permits.h"
#include "sonos/gena.h"

namespace coprocessor {

/// Runs on the Tether I/O task: each call becomes a handler on the reactor.
/// Cancelled calls need nothing here: their handlers find out when they
/// next send (and streams check while renewing their subscriptions).
class SonosServiceHandler final : public CoprocessorProto::sonos::Handler {
 public:
  /// HttpClients a call may use at once (WatchPlayback's two subscriptions
  /// and its position queries).
  static constexpr std::size_t kMaxHttpClientsPerCall = 3;

  SonosServiceHandler(Reactor& reactor, ConnectionPermits& permits,
                      GenaRegistry& registry)
      : reactor_(reactor), permits_(permits), registry_(registry) {}

  void watch_topology(
      tether::Sink<CoprocessorProto::TopologyUpdate> sink,
      const CoprocessorProto::TopologyRequest& request) override;

  void watch_playback(
      tether::Sink<CoprocessorProto::StateUpdate> sink,
      const CoprocessorProto::PlaybackRequest& request) override;

  void action(tether::Reply<CoprocessorProto::Empty> reply,
              const CoprocessorProto::ActionRequest& request) override;

  void set_volume(tether::Reply<CoprocessorProto::Empty> reply,
                  const CoprocessorProto::VolumeRequest& request) override;

  void get_album_art(tether::Sink<CoprocessorProto::AlbumArtChunk> sink,
                     const CoprocessorProto::AlbumArtRequest& request) override;

  void get_groups_summary(
      tether::Sink<CoprocessorProto::GroupSummary> sink,
      const CoprocessorProto::GroupsSummaryRequest& request) override;

  void discover(tether::Sink<CoprocessorProto::Speaker> sink,
                const CoprocessorProto::Empty& request) override;

 private:
  Reactor& reactor_;
  ConnectionPermits& permits_;
  GenaRegistry& registry_;
  bool discovering_ = false;  // Reactor only: see new_discover().
};

}  // namespace coprocessor
