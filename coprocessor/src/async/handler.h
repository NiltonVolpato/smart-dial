// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include "heap_log.h"

namespace coprocessor {

class Reactor;

/// One call's work, as a state machine: created on any task, handed over with
/// Reactor::start(), then run as callbacks (on the reactor, or inline on
/// whichever task delivers an event it registered for).
///
/// It owns itself and nobody tracks it. Only it deletes itself, with finish():
/// when its work is done, or when a send to its call reports Closed (the
/// client cancelled; tether's Reply/Sink say so, nobody else needs to know).
/// So when it finishes it's nowhere else: its members unregister their own
/// timers, fds and events (HttpClient does), and it must not finish while a
/// callback it posted is still waiting to run.
///
/// Parts running on other tasks touch only thread-safe data or its own.
class BaseHandler {
 public:
  virtual ~BaseHandler() = default;
  BaseHandler(const BaseHandler&) = delete;
  BaseHandler& operator=(const BaseHandler&) = delete;

 protected:
  /// May run on any task, so it must only copy data in.
  explicit BaseHandler(Reactor& reactor) : reactor_(reactor) {}

  /// The first callback, on the reactor.
  virtual void start() = 0;

  /// Deletes this handler: touch nothing of it afterwards.
  void finish() {
    delete this;
    log_heap("Handler finished");
  }

  [[nodiscard]] Reactor& reactor() const { return reactor_; }

 private:
  friend class Reactor;  // Runs start().

  Reactor& reactor_;
};

}  // namespace coprocessor
