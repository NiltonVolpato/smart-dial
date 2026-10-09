// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include "tether/descriptor.h"

namespace coprocessor {

class Reactor;

/// One call's work, as a state machine living on the reactor: created on any
/// task, handed over with Reactor::start(), then run only as callbacks there.
///
/// It owns itself and nobody tracks it. It ends by deleting itself (finish(),
/// or the answering helpers of UnaryHandler), or when its call is cancelled:
/// the reactor finds it through a callback it's waiting on and deletes it.
/// So a handler that isn't running must be waiting on the reactor (inbox,
/// timer or fd), never on anyone else. Its destructor drops whatever it was
/// still waiting on.
///
/// The constructor may run on any task, so it must only copy data in.
class BaseHandler {
 public:
  virtual ~BaseHandler();
  BaseHandler(const BaseHandler&) = delete;
  BaseHandler& operator=(const BaseHandler&) = delete;

  /// The call it works for; CallId{} for none, never cancelled.
  [[nodiscard]] tether::CallId call_id() const { return call_; }

 protected:
  BaseHandler(Reactor& reactor, tether::CallId call)
      : reactor_(reactor), call_(call) {}

  /// The first callback, on the reactor.
  virtual void start() = 0;

  /// Deletes this handler: touch nothing of it afterwards.
  void finish() { delete this; }

  [[nodiscard]] Reactor& reactor() const { return reactor_; }

 private:
  friend class Reactor;  // Runs start().

  Reactor& reactor_;
  tether::CallId call_;
};

}  // namespace coprocessor
