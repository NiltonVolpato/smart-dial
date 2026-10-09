// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// The Sonos service's streams: household topology, and one group's playback.
// Both subscribe to speaker events (GENA) and forward what they bring.

#pragma once

#include "tether/typed.h"

#include "async/handler.h"
#include "async/reactor.h"
#include "coprocessor_rpc.h"
#include "net/connection_permits.h"
#include "sonos/gena.h"

namespace coprocessor {

/// Tether I/O task: a handler for WatchTopology, to hand to Reactor::start().
BaseHandler* new_watch_topology(
    Reactor& reactor, ConnectionPermits& permits, GenaRegistry& registry,
    tether::Sink<CoprocessorProto::TopologyUpdate> sink,
    const CoprocessorProto::TopologyRequest& request);

/// Tether I/O task: a handler for WatchPlayback, to hand to Reactor::start().
BaseHandler* new_watch_playback(
    Reactor& reactor, ConnectionPermits& permits, GenaRegistry& registry,
    tether::Sink<CoprocessorProto::StateUpdate> sink,
    const CoprocessorProto::PlaybackRequest& request);

}  // namespace coprocessor
