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

/// Sends one stream item, from any task. Returns false once the stream is
/// over: the client cancelled or the link restarted (Closed), or the item was
/// too large (a bug: the stream is ended with INTERNAL). Stop sending then.
///
/// CAVEAT: on QueueFull/NoCredit the item is DROPPED, not retried: the client
/// keeps its previous value until the next item. Retrying in place could
/// deadlock on the I/O task, the only one draining the send queue; a retry
/// belongs in a handler, on the reactor.
template <class T, class Build>
[[nodiscard]] bool send_item(const tether::Sink<T>& sink, Build&& build) {
  auto result = sink.send(std::forward<Build>(build));
  if (result) {
    return true;
  }
  switch (result.error()) {
    case tether::CallError::Closed:
      return false;
    case tether::CallError::QueueFull:
    case tether::CallError::NoCredit:
      ESP_LOGW(detail::kRpcTag, "Item on call %u dropped: %s",
               static_cast<unsigned>(sink.call_id()),
               result.error() == tether::CallError::NoCredit
                   ? "client hasn't caught up"
                   : "send queue full");
      return true;
    case tether::CallError::TooLarge:
      ESP_LOGE(detail::kRpcTag, "Item on call %u exceeds the max payload",
               static_cast<unsigned>(sink.call_id()));
      // Ending can only fail as Closed or QueueFull: nothing left to do.
      (void)sink.end(tether::WireStatus::INTERNAL);
      return false;
  }
  return false;
}

}  // namespace coprocessor
