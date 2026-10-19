// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// Sonos discovery: speakers answer an mDNS query for _sonos._tcp.local.

#pragma once

#include "tether/typed.h"

#include "async/handler.h"
#include "async/reactor.h"
#include "coprocessor_rpc.h"

namespace coprocessor {

/// Tether I/O task: a handler for Discover, to hand to Reactor::start().
/// `running` is the reactor's: whether a discovery runs (one at a time: it
/// takes one of lwIP's few sockets).
BaseHandler* new_discover(Reactor& reactor, bool& running,
                          tether::Sink<CoprocessorProto::Speaker> sink);

}  // namespace coprocessor
