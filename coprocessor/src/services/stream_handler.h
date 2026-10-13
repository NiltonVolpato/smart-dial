// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <expected>
#include <optional>
#include <utility>

#include "tether/callback.h"
#include "tether/typed.h"

#include "async/handler.h"
#include "async/reactor.h"
#include "log.h"
#include "sonos/gena.h"

namespace coprocessor {

/// A handler feeding one stream: it sends items until it's done, or a send
/// finds the stream Closed (the client cancelled), then finishes.
///
/// A send that finds no room (no_room()) is retried in ready(), which runs
/// once there may be: tether calls back when an ack frees its send queue or
/// the client grants credit. It also calls back when the client cancels (or
/// the link goes down): then ready() if a send was waiting, else on_closed().
template <class Item>
class StreamHandler : public BaseHandler {
 protected:
  /// On any task (see BaseHandler), but while the call is open, i.e. in its
  /// dispatch: a cancel before on_ready() would go unnoticed.
  StreamHandler(Reactor& reactor, tether::Sink<Item> sink)
      : BaseHandler(reactor), sink_(sink) {
    sink_.on_ready(tether::Callback(this, &StreamHandler::post_ready));
  }

  /// Whether a send failed for want of room, to retry in ready(): in the link
  /// (QueueFull) or with the client (NoCredit).
  [[nodiscard]] static bool no_room(tether::CallError error) {
    return error == tether::CallError::QueueFull ||
           error == tether::CallError::NoCredit;
  }

  /// One item. On Closed, finish; on no_room(), ready() follows.
  template <class Build>
  std::expected<void, tether::CallError> send(Build&& build) {
    auto result = sink_.send(std::forward<Build>(build));
    waiting_ = !result && no_room(result.error());
    return result;
  }

  /// Reactor: after a send found no room, there may be some now, or the
  /// stream closed: send again, and its result tells which.
  virtual void ready() = 0;

  /// Reactor: the client cancelled while no send was waiting; closed() is
  /// true. By default nothing: the next send finds Closed.
  virtual void on_closed() {}

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

  /// Deletes this handler, now or, if a ready() is on its way to the reactor,
  /// once it gets there: touch nothing of it afterwards.
  void finish() {
    sink_.on_ready({});  // No ready() is handed over after this.
    if (ready_posted_) {
      finishing_ = true;
      return;
    }
    BaseHandler::finish();
  }

 private:
  // Tether's I/O task, holding the server's lock: hands ready() over.
  void post_ready() {
    if (!ready_posted_.exchange(true)) {
      this->reactor().post(tether::Callback(this, &StreamHandler::run_ready));
    }
  }

  void run_ready() {
    ready_posted_ = false;
    if (finishing_) {
      BaseHandler::finish();
    } else if (std::exchange(waiting_, false)) {
      ready();  // Not if a send went through meanwhile.
    } else if (closed()) {
      on_closed();
    }
  }

  tether::Sink<Item> sink_;
  std::atomic<bool> ready_posted_{false};
  bool finishing_ = false;
  bool waiting_ = false;  // The last send found no room.
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
      this->reactor().post(
          tether::Callback(this, &NotifiedStreamHandler::run_flush));
    }
  }

  /// Reactor: sends what NOTIFYs brought.
  virtual void flush() = 0;

  /// Reactor: stops NOTIFYs, then ends the stream with `status` (if the
  /// client is still there) and finishes. Nothing of this handler should
  /// run afterwards, so return at once.
  void stop(std::optional<tether::WireStatus> status) {
    if (stopping_) {
      return;
    }
    stopping_ = true;
    status_ = status;
    registry_.remove(this);
    this->reactor().post(
        tether::Callback(this, &NotifiedStreamHandler::finish_now));
  }

  [[nodiscard]] bool stopping() const { return stopping_; }

 private:
  // Subscriptions and clients go at once: a quiet speaker may not NOTIFY for
  // long, so the next send could be far off.
  void on_closed() override { stop(std::nullopt); }

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
};

}  // namespace coprocessor
