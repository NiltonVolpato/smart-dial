// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "async/reactor.h"

#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_vfs_eventfd.h"
#endif

#include "heap_log.h"

#include "async/handler.h"
#include "log.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "reactor";

// Longer waits block in select() without a timeout.
constexpr auto kForever = std::chrono::hours(1);

// A TimerId is (generation << kSlotBits) | slot.
constexpr unsigned kSlotBits = 8;
constexpr uint32_t kSlotMask = (1U << kSlotBits) - 1;
constexpr uint32_t kGenerationMask = (1U << (32 - kSlotBits)) - 1;

}  // namespace

Reactor::Reactor(Capacity capacity)
    : inbox_(capacity.inbox),
      watches_(capacity.watches),
      timers_(capacity.timers) {
  if (capacity.inbox == 0 || capacity.timers > kSlotMask + 1) {
    ESP_LOGE(TAG, "Bad capacity: inbox %u, timers %u (at most %u)",
             static_cast<unsigned>(capacity.inbox),
             static_cast<unsigned>(capacity.timers), kSlotMask + 1);
    std::abort();
  }
  // Never grow past these: no allocation after construction.
  heap_.reserve(capacity.timers);
  free_timers_.reserve(capacity.timers);
  for (std::size_t slot = capacity.timers; slot-- > 0;) {
    free_timers_.push_back(slot);
  }
#ifdef ESP_PLATFORM
  wake_read_fd_ = eventfd(0, 0);
  wake_write_fd_ = wake_read_fd_;
  if (wake_read_fd_ < 0) {
    ESP_LOGE(TAG, "eventfd failed (errno=%d)", errno);
    std::abort();
  }
#else
  int fds[2];
  if (pipe(fds) != 0) {
    ESP_LOGE(TAG, "pipe failed (errno=%d)", errno);
    std::abort();
  }
  wake_read_fd_ = fds[0];
  wake_write_fd_ = fds[1];
  fcntl(wake_read_fd_, F_SETFL, fcntl(wake_read_fd_, F_GETFL) | O_NONBLOCK);
  fcntl(wake_write_fd_, F_SETFL, fcntl(wake_write_fd_, F_GETFL) | O_NONBLOCK);
#endif
}

Reactor::~Reactor() {
  close(wake_read_fd_);
  if (wake_write_fd_ != wake_read_fd_) {
    close(wake_write_fd_);
  }
}

void Reactor::start(BaseHandler* handler) {
  log_heap("Handler created");
  post(tether::Callback(handler, &BaseHandler::start));
}

void Reactor::stop() {
  {
    std::scoped_lock lock(mutex_);
    stopped_ = true;
  }
  wake();
}

void Reactor::post(tether::Callback callback) {
  bool was_empty = false;
  {
    std::scoped_lock lock(mutex_);
    if (inbox_size_ == inbox_.size()) {
      ESP_LOGE(TAG, "Inbox full (%u callbacks)",
               static_cast<unsigned>(inbox_.size()));
      std::abort();
    }
    inbox_[(inbox_head_ + inbox_size_) % inbox_.size()] = callback;
    was_empty = inbox_size_++ == 0;
  }
  // A non-empty inbox already has a wake-up pending.
  if (was_empty) {
    wake();
  }
}

bool Reactor::pop(tether::Callback* callback) {
  std::scoped_lock lock(mutex_);
  if (inbox_size_ == 0) {
    return false;
  }
  *callback = inbox_[inbox_head_];
  inbox_head_ = (inbox_head_ + 1) % inbox_.size();
  --inbox_size_;
  return true;
}

void Reactor::run() {
  for (;;) {
    {
      std::scoped_lock lock(mutex_);
      if (stopped_) {
        return;
      }
    }
    run_once(Clock::duration::max());
  }
}

void Reactor::run_once(Clock::duration max_wait) {
  Clock::duration wait = max_wait;
  {
    std::scoped_lock lock(mutex_);
    if (inbox_size_ > 0) {
      wait = Clock::duration::zero();
    }
  }
  if (!heap_.empty()) {
    wait = std::min(wait, std::max(Clock::duration::zero(),
                                   timers_[heap_[0]].deadline - Clock::now()));
  }

  fd_set readable;
  fd_set writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_SET(wake_read_fd_, &readable);
  int max_fd = wake_read_fd_;
  for (const Watch& watch : watches_) {
    if (watch.fd < 0) {
      continue;
    }
    if (watch.readable) {
      FD_SET(watch.fd, &readable);
    }
    if (watch.writable) {
      FD_SET(watch.fd, &writable);
    }
    max_fd = std::max(max_fd, watch.fd);
  }

  timeval timeout{};
  timeval* timeout_ptr = nullptr;
  if (wait < kForever) {
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(wait);
    timeout.tv_sec =
        static_cast<decltype(timeout.tv_sec)>(micros.count() / 1'000'000);
    timeout.tv_usec =
        static_cast<decltype(timeout.tv_usec)>(micros.count() % 1'000'000);
    timeout_ptr = &timeout;
  }

  int ready = select(max_fd + 1, &readable, &writable, nullptr, timeout_ptr);
  if (ready < 0) {
    if (errno == EBADF) {
      // Someone closed an fd without forget(): a bug, but don't spin on it.
      drop_bad_fds();
    } else if (errno != EINTR) {
      ESP_LOGE(TAG, "select failed (errno=%d)", errno);
    }
  } else if (ready > 0) {
    if (FD_ISSET(wake_read_fd_, &readable)) {
      drain_wake();
    }
    dispatch_fds(readable, writable);
  }

  run_inbox();
  run_timers();
}

void Reactor::run_inbox() {
  std::size_t count = 0;
  {
    std::scoped_lock lock(mutex_);
    count = inbox_size_;
  }
  // Only what was there when the iteration began; the lock is released while
  // each callback runs, so other tasks never wait on it.
  tether::Callback callback;
  for (std::size_t i = 0; i < count && pop(&callback); ++i) {
    callback();
  }
}

Reactor::TimerId Reactor::after(Clock::duration delay,
                                tether::Callback callback) {
  if (free_timers_.empty()) {
    ESP_LOGE(TAG, "Too many timers (%u)",
             static_cast<unsigned>(timers_.size()));
    std::abort();
  }
  const std::size_t slot = free_timers_.back();
  free_timers_.pop_back();

  const TimerId id = (next_generation_ << kSlotBits) | slot;
  next_generation_ = (next_generation_ + 1) & kGenerationMask;
  if (next_generation_ == 0) {
    next_generation_ = 1;  // Keeps ids non-zero.
  }

  Timer& timer = timers_[slot];
  timer.deadline = Clock::now() + delay;
  timer.id = id;
  timer.sequence = next_sequence_++;
  timer.callback = callback;
  heap_.push_back(slot);
  timer.heap_index = heap_.size() - 1;
  sift_up(timer.heap_index);
  return id;
}

void Reactor::cancel_timer(TimerId id) {
  const std::size_t slot = id & kSlotMask;
  if (id != 0 && slot < timers_.size() && timers_[slot].id == id) {
    remove_timer(slot);
  }
}

bool Reactor::earlier(std::size_t a, std::size_t b) const {
  const Timer& x = timers_[a];
  const Timer& y = timers_[b];
  return x.deadline != y.deadline ? x.deadline < y.deadline
                                  : x.sequence < y.sequence;
}

void Reactor::heap_place(std::size_t heap_index, std::size_t slot) {
  heap_[heap_index] = slot;
  timers_[slot].heap_index = heap_index;
}

void Reactor::sift_up(std::size_t heap_index) {
  while (heap_index > 0) {
    const std::size_t parent = (heap_index - 1) / 2;
    if (!earlier(heap_[heap_index], heap_[parent])) {
      return;
    }
    const std::size_t slot = heap_[heap_index];
    heap_place(heap_index, heap_[parent]);
    heap_place(parent, slot);
    heap_index = parent;
  }
}

void Reactor::sift_down(std::size_t heap_index) {
  for (;;) {
    std::size_t first = heap_index;
    for (std::size_t child : {2 * heap_index + 1, 2 * heap_index + 2}) {
      if (child < heap_.size() && earlier(heap_[child], heap_[first])) {
        first = child;
      }
    }
    if (first == heap_index) {
      return;
    }
    const std::size_t slot = heap_[heap_index];
    heap_place(heap_index, heap_[first]);
    heap_place(first, slot);
    heap_index = first;
  }
}

void Reactor::remove_timer(std::size_t slot) {
  const std::size_t index = timers_[slot].heap_index;
  const std::size_t last = heap_.size() - 1;
  if (index != last) {
    // The last entry fills the hole, then moves whichever way it belongs.
    heap_place(index, heap_[last]);
  }
  heap_.pop_back();
  if (index < heap_.size()) {
    if (index > 0 && earlier(heap_[index], heap_[(index - 1) / 2])) {
      sift_up(index);
    } else {
      sift_down(index);
    }
  }
  timers_[slot] = {};
  free_timers_.push_back(slot);
}

void Reactor::when_readable(int fd, tether::Callback callback) {
  watch_slot(fd).readable = callback;
}

void Reactor::when_writable(int fd, tether::Callback callback) {
  watch_slot(fd).writable = callback;
}

Reactor::Watch& Reactor::watch_slot(int fd) {
  Watch* free_slot = nullptr;
  for (Watch& watch : watches_) {
    if (watch.fd == fd) {
      return watch;
    }
    if (watch.fd < 0 && free_slot == nullptr) {
      free_slot = &watch;
    }
  }
  if (free_slot == nullptr) {
    ESP_LOGE(TAG, "Too many watched fds (%u)",
             static_cast<unsigned>(watches_.size()));
    std::abort();
  }
  free_slot->fd = fd;
  return *free_slot;
}

void Reactor::forget(int fd) {
  for (Watch& watch : watches_) {
    if (watch.fd == fd) {
      watch = {};
      return;
    }
  }
}

void Reactor::wake() {
#ifdef ESP_PLATFORM
  const uint64_t one = 1;
#else
  const uint8_t one = 1;
#endif
  // Failure means a wake-up is already pending (full pipe): nothing to do.
  (void)write(wake_write_fd_, &one, sizeof(one));
}

void Reactor::drain_wake() {
#ifdef ESP_PLATFORM
  // ESP-IDF's eventfd read never blocks: it returns the count and resets it.
  uint64_t count = 0;
  (void)read(wake_read_fd_, &count, sizeof(count));
#else
  uint8_t buf[64];
  while (read(wake_read_fd_, buf, sizeof(buf)) > 0) {
  }
#endif
}

void Reactor::dispatch_fds(const fd_set& readable, const fd_set& writable) {
  // Slots never move, so callbacks may add or forget watches as we go.
  for (Watch& watch : watches_) {
    for (bool want_read : {true, false}) {
      // Read again each time: the previous callback may have changed it.
      const int fd = watch.fd;
      if (fd < 0 || !FD_ISSET(fd, want_read ? &readable : &writable)) {
        continue;
      }
      tether::Callback& slot = want_read ? watch.readable : watch.writable;
      if (!slot) {
        continue;
      }
      tether::Callback callback = std::exchange(slot, tether::Callback{});
      if (!watch.readable && !watch.writable) {
        watch = {};
      }
      callback();
    }
  }
}

void Reactor::run_timers() {
  const Clock::time_point now = Clock::now();
  // Timers armed by these callbacks wait for the next iteration, even with
  // zero delay, so a timer re-arming itself can't starve the loop. They sort
  // after every older due timer, so the first one ends the pass.
  const uint32_t first_new = next_sequence_;
  while (!heap_.empty()) {
    const std::size_t slot = heap_[0];
    const Timer& timer = timers_[slot];
    const bool is_new = static_cast<int32_t>(timer.sequence - first_new) >= 0;
    if (timer.deadline > now || is_new) {
      return;
    }
    tether::Callback callback = timer.callback;
    remove_timer(slot);
    callback();
  }
}

void Reactor::drop_bad_fds() {
  for (Watch& watch : watches_) {
    if (watch.fd >= 0 && fcntl(watch.fd, F_GETFL) < 0) {
      ESP_LOGE(TAG, "fd %d was closed while watched; dropping", watch.fd);
      watch = {};
    }
  }
}

}  // namespace coprocessor
