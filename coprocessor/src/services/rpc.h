// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// Answering unary calls, with send failures handled in one place.

#pragma once

#include <utility>

#include "tether/typed.h"

#include "log.h"

namespace coprocessor {

namespace detail {

inline constexpr const char* kRpcTag = "rpc";

// Logs what a failed reply means. Returns true for TooLarge: the caller
// should answer INTERNAL instead.
inline bool reply_failed(tether::CallId call, tether::CallError error) {
  switch (error) {
    case tether::CallError::Closed:
      return false;  // Client cancelled or link restarted: nobody to answer.
    case tether::CallError::QueueFull:
    case tether::CallError::NoCredit:
      ESP_LOGW(kRpcTag, "Reply to call %u dropped: send queue full",
               static_cast<unsigned>(call));
      return false;
    case tether::CallError::TooLarge:
      ESP_LOGE(kRpcTag, "Reply to call %u exceeds the max payload",
               static_cast<unsigned>(call));
      return true;
  }
  return false;
}

}  // namespace detail

/// Answers a unary call, from any task.
///
/// CAVEAT: on QueueFull/NoCredit the reply is DROPPED, not retried: the client
/// gets no answer and waits for its own timeout. Retrying in place could
/// deadlock on the I/O task, the only one draining the send queue; a retry
/// belongs in a Handler, on the reactor.
template <class T, class Build>
void send_reply(const tether::Reply<T>& reply, Build&& build) {
  auto result = reply.send(std::forward<Build>(build));
  if (!result && detail::reply_failed(reply.call_id(), result.error())) {
    // An error status is tiny: this can only fail as Closed or QueueFull,
    // and there's nothing left to do then.
    (void)reply.fail(tether::WireStatus::INTERNAL);
  }
}

/// Answers a unary call with an error status. Failures as for send_reply.
template <class T>
void fail_reply(const tether::Reply<T>& reply, tether::WireStatus status) {
  auto result = reply.fail(status);
  if (!result) {
    detail::reply_failed(reply.call_id(), result.error());
  }
}

}  // namespace coprocessor
