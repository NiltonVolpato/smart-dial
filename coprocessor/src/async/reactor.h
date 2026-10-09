// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <sys/select.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "tether/descriptor.h"

#include "async/callback.h"

namespace coprocessor {

class BaseHandler;

/// Single-threaded event loop running callbacks: work handed over by other
/// tasks, timers and socket readiness, all on the one task that calls run().
/// start(), post(), cancel() and stop() may be called from any task;
/// everything else only from the reactor task, i.e. from a callback.
///
/// The reactor doesn't track handlers. Every pending callback knows its owner,
/// so cancel() finds a call's handler by scanning what's pending, and a
/// handler's destructor drops what it owned via forget(owner).
class Reactor {
 public:
  using Clock = std::chrono::steady_clock;
  /// Never 0. A slot plus a generation: a stale id never matches a reused slot.
  using TimerId = uint32_t;

  /// Everything the reactor will ever hold, allocated once at construction.
  /// Exceeding any of these aborts: size them from the workload.
  struct Capacity {
    /// Handed over by other tasks and not run yet (e.g. a start and a cancel
    /// per concurrent call).
    std::size_t inbox;
    /// Fds watched at once (e.g. a socket per concurrent call).
    std::size_t watches;
    /// Timers pending at once, at most 256.
    std::size_t timers;
  };

  /// On ESP-IDF, requires esp_vfs_eventfd_register() to have been called.
  explicit Reactor(Capacity capacity);
  ~Reactor();
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  // --- Any task ---

  /// Runs `handler`'s start() on the reactor, which then owns it.
  void start(BaseHandler* handler);
  /// Runs `callback` on the reactor, in posting order.
  void post(Callback callback);
  /// Deletes `call`'s handler, wherever it's waiting. A no-op if it already
  /// finished.
  void cancel(tether::CallId call);
  /// Makes run() return after its current iteration.
  void stop();

  // --- Reactor task ---

  /// Runs until stop().
  void run();
  /// One iteration: waits at most `max_wait` for an fd, a timer or the
  /// inbox, then runs whatever is ready.
  void run_once(Clock::duration max_wait);

  /// Runs `callback` once, after `delay`.
  TimerId after(Clock::duration delay, Callback callback);
  /// No-op for a timer that already fired or was cancelled.
  void cancel_timer(TimerId id);

  /// One-shot: runs `callback` once `fd` is readable, at EOF or in error.
  void when_readable(int fd, Callback callback);
  /// One-shot: runs `callback` once `fd` is writable or in error (e.g. a
  /// non-blocking connect finished).
  void when_writable(int fd, Callback callback);
  /// Drops `fd`'s pending callbacks. Call before closing it. A watch may still
  /// fire spuriously for a reused fd number, so I/O must tolerate EAGAIN.
  void forget(int fd);

  /// Drops every pending callback `owner` owns: inbox, timers and fds.
  void forget(BaseHandler* owner);

 private:
  // A callback to run or, when empty, a call to cancel.
  struct Message {
    Callback callback;
    tether::CallId cancel{};
  };
  struct Watch {
    int fd = -1;  // -1: a free slot.
    Callback readable;
    Callback writable;
  };
  struct Timer {
    Clock::time_point deadline;
    TimerId id = 0;              // 0: a free slot.
    uint32_t sequence = 0;       // Breaks deadline ties in arming order.
    std::size_t heap_index = 0;  // Where heap_ refers to this slot.
    Callback callback;
  };

  void push(Message message);
  bool pop(Message* message);
  void run_inbox();
  void cancel_now(tether::CallId call);
  BaseHandler* find_owner(tether::CallId call);
  void wake();
  void drain_wake();
  void dispatch_fds(const fd_set& readable, const fd_set& writable);
  Watch& watch_slot(int fd);
  void run_timers();
  void drop_bad_fds();

  // Binary min-heap over timer slots, by (deadline, sequence).
  [[nodiscard]] bool earlier(std::size_t a, std::size_t b) const;
  void heap_place(std::size_t heap_index, std::size_t slot);
  void sift_up(std::size_t heap_index);
  void sift_down(std::size_t heap_index);
  void remove_timer(std::size_t slot);

  // Wake-up channel for the inbox and stop(): eventfd on ESP-IDF (one fd), a
  // pipe elsewhere.
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;

  // The only state shared with other tasks.
  std::mutex mutex_;
  // Handed over and not run yet, in order: a ring allocated once, at
  // construction. Each iteration runs only what was there when it began, so
  // work posting more work can't starve the loop.
  std::vector<Message> inbox_;  // Guarded by mutex_.
  std::size_t inbox_head_ = 0;  // Guarded by mutex_.
  std::size_t inbox_size_ = 0;  // Guarded by mutex_.
  bool stopped_ = false;        // Guarded by mutex_.

  // Reactor task only from here on.

  // Pending callbacks per fd, in fixed slots. A linear scan: there are only a
  // handful, and it also builds select()'s fd sets.
  std::vector<Watch> watches_;

  // Timers live in fixed slots, found from their id in O(1). heap_ orders the
  // used slots by deadline, so heap_[0] is the next to fire: it bounds
  // select()'s wait. Cancelling removes from the middle in O(log n).
  std::vector<Timer> timers_;
  std::vector<std::size_t> heap_;  // Slot indices; size() == timers armed.
  std::vector<std::size_t> free_timers_;  // Free slot indices, as a stack.
  uint32_t next_generation_ = 1;
  uint32_t next_sequence_ = 0;
};

}  // namespace coprocessor
