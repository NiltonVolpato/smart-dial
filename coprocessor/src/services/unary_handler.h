// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <utility>

#include "tether/typed.h"

#include "async/handler.h"
#include "services/rpc.h"

namespace coprocessor {

/// A handler answering one unary call. The reply is private, and answering
/// is the only way to finish: a handler that forgets to finish has also never
/// answered, so the client notices (it times out) instead of a silent leak.
/// A cancelled call needs nothing special: the answer finds it Closed.
template <class Response>
class UnaryHandler : public BaseHandler {
 protected:
  UnaryHandler(Reactor& reactor, tether::Reply<Response> reply)
      : BaseHandler(reactor), reply_(reply) {}

  /// Answers the call, then deletes this handler: touch nothing of it after.
  template <class Build>
  void reply_and_finish(Build&& build) {
    send_reply(reply_, std::forward<Build>(build));
    finish();
  }

  /// Answers the call with an error, then deletes this handler.
  void fail_and_finish(tether::WireStatus status) {
    fail_reply(reply_, status);
    finish();
  }

 private:
  using BaseHandler::finish;  // Not for subclasses: they must answer.

  tether::Reply<Response> reply_;
};

}  // namespace coprocessor
