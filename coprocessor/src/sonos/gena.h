// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// UPnP GENA eventing: subscriptions to a speaker's state variables, and the
// NOTIFYs it then sends to the web server's /media/notify.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include "async/callback.h"
#include "async/reactor.h"
#include "net/connection_permits.h"
#include "net/http_client.h"
#include "net/http_request.h"
#include "net/ipv4.h"

namespace coprocessor {

/// Receives one subscription's NOTIFYs, inline on the web server's task, as
/// their body streams in. Touch only thread-safe data or your own.
class NotifyListener {
 public:
  /// Returns false if the body is malformed (the speaker gets a 400).
  virtual bool on_notify(std::string_view chunk, bool first, bool last) = 0;

 protected:
  ~NotifyListener() = default;
};

/// Who listens to which subscription (by SID). The web server delivers each
/// NOTIFY under its lock, so remove() also waits for one being delivered:
/// after it, the listener is never called again.
class GenaRegistry {
 public:
  /// Subscriptions at once; more aborts. Allocated here, once.
  explicit GenaRegistry(std::size_t capacity);
  GenaRegistry(const GenaRegistry&) = delete;
  GenaRegistry& operator=(const GenaRegistry&) = delete;

  // Any task.
  void add(std::string_view sid, NotifyListener* listener);
  /// Every SID of `listener`'s.
  void remove(NotifyListener* listener);
  void remove(std::string_view sid);

  /// Web server task: runs `deliver(listener)` for `sid`'s listener, under
  /// the lock. False for an unknown SID (an old or foreign subscription).
  template <class Deliver>
  bool dispatch(std::string_view sid, Deliver&& deliver) {
    std::scoped_lock lock(mutex_);
    for (Entry& entry : entries_) {
      if (entry.listener != nullptr && entry.sid() == sid) {
        deliver(*entry.listener);
        return true;
      }
    }
    return false;
  }

 private:
  struct Entry {
    std::array<char, 96> text{};  // Sonos SIDs are about 60 characters.
    std::size_t size = 0;
    NotifyListener* listener = nullptr;
    [[nodiscard]] std::string_view sid() const { return {text.data(), size}; }
  };

  std::mutex mutex_;
  std::vector<Entry> entries_;  // Guarded by mutex_.
};

/// Where speakers send NOTIFYs: this device's IP, port 80, unless set here
/// (QEMU's host forwards them to its guest).
void gena_set_callback(Ipv4 host, uint16_t port);

/// One subscription to a speaker's events, e.g. AVTransport's. Lives in a
/// handler, on the reactor. While subscribed its NOTIFYs go to the listener;
/// destroying it stops them and unsubscribes (in the background).
class GenaSubscription final : public HttpResponseSink {
 public:
  GenaSubscription(Reactor& reactor, ConnectionPermits& permits,
                   GenaRegistry& registry, NotifyListener& listener,
                   Endpoint speaker, std::string_view event_path);
  ~GenaSubscription();
  GenaSubscription(const GenaSubscription&) = delete;
  GenaSubscription& operator=(const GenaSubscription&) = delete;

  /// Subscribes, or renews if subscribed (subscribing anew if the speaker
  /// forgot us: HTTP 412). `done` runs once it's over; check subscribed().
  void refresh(Callback done);

  [[nodiscard]] bool subscribed() const { return !sid().empty(); }
  /// When to renew: half the timeout the speaker granted.
  [[nodiscard]] std::chrono::seconds renew_after() const;

 private:
  enum class Step : uint8_t { Subscribe, Renew };

  void send(Step step);
  void sent();
  void on_header(std::string_view name, std::string_view value) override;
  [[nodiscard]] std::string_view sid() const {
    return {sid_.data(), sid_size_};
  }
  void set_sid(std::string_view sid);

  Reactor& reactor_;
  ConnectionPermits& permits_;
  GenaRegistry& registry_;
  NotifyListener& listener_;
  Endpoint speaker_;
  std::string_view event_path_;  // A literal.
  HttpClient http_;
  FixedRequest<512> request_;
  Step step_ = Step::Subscribe;
  Callback done_;

  std::array<char, 96> sid_{};  // Empty while not subscribed.
  std::size_t sid_size_ = 0;
  uint32_t granted_seconds_ = 0;
};

}  // namespace coprocessor
