// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <optional>
#include <utility>

#include "tether/typed.h"

#include "async/callback.h"
#include "async/handler.h"
#include "async/reactor.h"
#include "log.h"
#include "sonos/gena.h"

namespace coprocessor {

/// A handler feeding one stream: it sends items until it's done, or a send
/// finds the stream Closed (the client cancelled), then finishes.
template <class Item>
class StreamHandler : public BaseHandler {
 protected:
  StreamHandler(Reactor& reactor, tether::Sink<Item> sink)
      : BaseHandler(reactor), sink_(sink) {}

  /// One item. On Closed, finish; on QueueFull/NoCredit, retry later.
  template <class Build>
  std::expected<void, tether::CallError> send(Build&& build) {
    return sink_.send(std::forward<Build>(build));
  }

  /// Whether the client is gone: once true, stays true.
  [[nodiscard]] bool closed() const { return sink_.closed(); }

  /// Ends the stream with `status`, then deletes this handler.
  void end_and_finish(tether::WireStatus status) {
    auto result = sink_.end(status);
    if (!result && result.error() != tether::CallError::Closed) {
      ESP_LOGW("stream", "Ending call %u failed (%d): its client times out",
               static_cast<unsigned>(sink_.call_id()),
               static_cast<int>(result.error()));
    }
    finish();
  }

 private:
  tether::Sink<Item> sink_;
};

/// A stream fed by GENA NOTIFYs. They're parsed inline on the web server's
/// task (on_notify), which then has the reactor send the result (flush()).
///
/// Ending is ordered: stop() first leaves the registry (so no NOTIFY runs
/// any more), then posts the finish. A flush the web server posted before
/// runs first: the inbox is in order. So nothing runs on a deleted handler.
template <class Item>
class NotifiedStreamHandler : public StreamHandler<Item>,
                              public NotifyListener {
 protected:
  NotifiedStreamHandler(Reactor& reactor, GenaRegistry& registry,
                        tether::Sink<Item> sink)
      : StreamHandler<Item>(reactor, sink), registry_(registry) {}
  ~NotifiedStreamHandler() override { registry_.remove(this); }

  /// Web server task: has the reactor run flush(), once however many
  /// NOTIFYs arrive before it does.
  void request_flush() {
    if (!flush_pending_.exchange(true)) {
      this->reactor().post(Callback(this, &NotifiedStreamHandler::run_flush));
    }
  }

  /// Reactor: sends what NOTIFYs brought.
  virtual void flush() = 0;

  /// Reactor: checks every kClosedCheck whether the client cancelled, and
  /// stops then. Its subscriptions and clients would otherwise wait for the
  /// next send to find out, and a quiet speaker may not NOTIFY for long.
  /// Call from start().
  void stop_when_closed() {
    closed_timer_ = this->reactor().after(
        kClosedCheck, Callback(this, &NotifiedStreamHandler::check_closed));
  }

  /// Reactor: stops NOTIFYs, then ends the stream with `status` (if the
  /// client is still there) and finishes. Nothing of this handler should
  /// run afterwards, so return at once.
  void stop(std::optional<tether::WireStatus> status) {
    if (stopping_) {
      return;
    }
    stopping_ = true;
    this->reactor().cancel_timer(closed_timer_);
    status_ = status;
    registry_.remove(this);
    this->reactor().post(Callback(this, &NotifiedStreamHandler::finish_now));
  }

  [[nodiscard]] bool stopping() const { return stopping_; }

 private:
  // 10 ticks of FreeRTOS (CONFIG_FREERTOS_HZ=100): a flag read each time.
  static constexpr std::chrono::milliseconds kClosedCheck{100};

  void check_closed() {
    closed_timer_ = 0;
    if (this->closed()) {
      stop(std::nullopt);
    } else {
      stop_when_closed();
    }
  }

  void run_flush() {
    flush_pending_ = false;
    if (!stopping_) {
      flush();
    }
  }

  void finish_now() {
    if (status_) {
      this->end_and_finish(*status_);
    } else {
      this->finish();
    }
  }

  GenaRegistry& registry_;
  std::atomic<bool> flush_pending_{false};
  bool stopping_ = false;
  std::optional<tether::WireStatus> status_;
  Reactor::TimerId closed_timer_ = 0;
};

}  // namespace coprocessor
