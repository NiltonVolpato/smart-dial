// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "tether/callback.h"

#include "async/reactor.h"

namespace coprocessor {
namespace {

using std::chrono::milliseconds;

constexpr Reactor::Capacity kCapacity{.inbox = 8, .watches = 4, .timers = 4};

class SocketPair {
 public:
  SocketPair() { EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0); }
  ~SocketPair() {
    close(fds_[0]);
    close(fds_[1]);
  }
  int a() const { return fds_[0]; }
  int b() const { return fds_[1]; }

 private:
  int fds_[2] = {-1, -1};
};

// Records which of its methods ran, in order.
struct Recorder {
  std::vector<int> log;
  void one() { log.push_back(1); }
  void two() { log.push_back(2); }
  tether::Callback callback(void (Recorder::*method)()) { return {this, method}; }
};

TEST(ReactorTest, PostedCallbacksRunInOrder) {
  Reactor reactor(kCapacity);
  Recorder recorder;
  reactor.post(recorder.callback(&Recorder::one));
  reactor.post(recorder.callback(&Recorder::two));
  reactor.run_once(milliseconds(0));
  EXPECT_EQ(recorder.log, (std::vector<int>{1, 2}));
}

TEST(ReactorTest, CallbackPostedByCallbackRunsNextIteration) {
  struct Reposter {
    Reactor& reactor;
    int runs = 0;
    void run() {
      if (++runs == 1) {
        reactor.post({this, &Reposter::run});
      }
    }
  };
  Reactor reactor(kCapacity);
  Reposter reposter{reactor};
  reactor.post({&reposter, &Reposter::run});
  reactor.run_once(milliseconds(0));
  EXPECT_EQ(reposter.runs, 1);
  reactor.run_once(milliseconds(0));
  EXPECT_EQ(reposter.runs, 2);
}

TEST(ReactorTest, PostFromAnotherThreadWakesRun) {
  struct Stopper {
    Reactor& reactor;
    std::thread::id ran_on;
    void run() {
      ran_on = std::this_thread::get_id();
      reactor.stop();
    }
  };
  Reactor reactor(kCapacity);
  Stopper stopper{reactor};
  std::thread poster([&] {
    std::this_thread::sleep_for(milliseconds(20));
    reactor.post({&stopper, &Stopper::run});
  });
  reactor.run();  // Blocks until the posted callback stops it.
  poster.join();
  EXPECT_EQ(stopper.ran_on, std::this_thread::get_id());
}

TEST(ReactorTest, FullInboxAborts) {
  Reactor reactor({.inbox = 1, .watches = 1, .timers = 1});
  Recorder recorder;
  reactor.post(recorder.callback(&Recorder::one));
  EXPECT_DEATH(reactor.post(recorder.callback(&Recorder::one)), "");
}

TEST(ReactorTest, TimersFireInDeadlineOrder) {
  Reactor reactor(kCapacity);
  Recorder recorder;
  reactor.after(milliseconds(20), recorder.callback(&Recorder::two));
  reactor.after(milliseconds(5), recorder.callback(&Recorder::one));
  auto deadline = Reactor::Clock::now() + milliseconds(500);
  while (recorder.log.size() < 2 && Reactor::Clock::now() < deadline) {
    reactor.run_once(milliseconds(50));
  }
  EXPECT_EQ(recorder.log, (std::vector<int>{1, 2}));
}

TEST(ReactorTest, CancelledTimerDoesNotFire) {
  Reactor reactor(kCapacity);
  Recorder recorder;
  Reactor::TimerId id =
      reactor.after(milliseconds(0), recorder.callback(&Recorder::one));
  reactor.cancel_timer(id);
  reactor.run_once(milliseconds(10));
  EXPECT_TRUE(recorder.log.empty());
}

TEST(ReactorTest, TooManyTimersAborts) {
  Reactor reactor({.inbox = 1, .watches = 1, .timers = 1});
  Recorder recorder;
  reactor.after(milliseconds(10), recorder.callback(&Recorder::one));
  EXPECT_DEATH(
      reactor.after(milliseconds(10), recorder.callback(&Recorder::one)), "");
}

TEST(ReactorTest, TimerSlotsAreReused) {
  Reactor reactor({.inbox = 1, .watches = 1, .timers = 1});
  Recorder recorder;
  for (int i = 0; i < 3; ++i) {  // More timers than slots, one at a time.
    reactor.after(milliseconds(0), recorder.callback(&Recorder::one));
    reactor.run_once(milliseconds(10));
  }
  EXPECT_EQ(recorder.log.size(), 3U);
}

TEST(ReactorTest, StaleTimerIdDoesNotCancelSlotsNewTimer) {
  Reactor reactor({.inbox = 1, .watches = 1, .timers = 1});
  Recorder recorder;
  Reactor::TimerId stale =
      reactor.after(milliseconds(0), recorder.callback(&Recorder::one));
  reactor.run_once(milliseconds(10));  // Fires: the slot is free again.
  Reactor::TimerId fresh =
      reactor.after(milliseconds(0), recorder.callback(&Recorder::two));
  EXPECT_NE(stale, fresh);
  reactor.cancel_timer(stale);
  reactor.run_once(milliseconds(10));
  EXPECT_EQ(recorder.log, (std::vector<int>{1, 2}));
}

TEST(ReactorTest, HeapKeepsDeadlineOrderThroughCancels) {
  // Records the deadline each timer was armed with, as it fires.
  struct Probe {
    std::vector<int>* fired;
    int deadline_ms;
    void fire() { fired->push_back(deadline_ms); }
  };
  constexpr int kTimers = 32;
  Reactor reactor({.inbox = 1, .watches = 1, .timers = kTimers});
  std::vector<int> fired;
  std::vector<Probe> probes;
  probes.reserve(kTimers);
  std::vector<Reactor::TimerId> ids;
  std::vector<int> expected;
  uint32_t seed = 12345;  // A fixed pseudo-random sequence.
  for (int i = 0; i < kTimers; ++i) {
    seed = seed * 1103515245 + 12345;
    int deadline_ms = static_cast<int>((seed >> 16) % 60);
    probes.push_back({&fired, deadline_ms});
    ids.push_back(reactor.after(milliseconds(deadline_ms),
                                {&probes.back(), &Probe::fire}));
  }
  // Cancel every third, from the middle of the heap as much as the ends.
  for (int i = 0; i < kTimers; ++i) {
    if (i % 3 == 0) {
      reactor.cancel_timer(ids[i]);
    } else {
      expected.push_back(probes[i].deadline_ms);
    }
  }
  std::ranges::sort(expected);
  auto deadline = Reactor::Clock::now() + milliseconds(1000);
  while (fired.size() < expected.size() && Reactor::Clock::now() < deadline) {
    reactor.run_once(milliseconds(5));
  }
  EXPECT_EQ(fired, expected);
}

TEST(ReactorTest, ZeroDelayTimerArmedByTimerWaitsForNextIteration) {
  struct Rearmer {
    Reactor& reactor;
    int fires = 0;
    void fire() {
      ++fires;
      reactor.after(milliseconds(0), {this, &Rearmer::fire});
    }
  };
  Reactor reactor(kCapacity);
  Rearmer rearmer{reactor};
  reactor.after(milliseconds(0), {&rearmer, &Rearmer::fire});
  reactor.run_once(milliseconds(0));
  EXPECT_EQ(rearmer.fires, 1);
}

TEST(ReactorTest, ReadableFiresOnceWhenDataArrives) {
  Reactor reactor(kCapacity);
  SocketPair pair;
  Recorder recorder;
  reactor.when_readable(pair.a(), recorder.callback(&Recorder::one));

  reactor.run_once(milliseconds(0));
  EXPECT_TRUE(recorder.log.empty());

  ASSERT_EQ(write(pair.b(), "x", 1), 1);
  reactor.run_once(milliseconds(100));
  EXPECT_EQ(recorder.log.size(), 1U);

  // One-shot: still readable, but nobody is watching.
  reactor.run_once(milliseconds(0));
  EXPECT_EQ(recorder.log.size(), 1U);
}

TEST(ReactorTest, WritableFiresForConnectedSocket) {
  Reactor reactor(kCapacity);
  SocketPair pair;
  Recorder recorder;
  reactor.when_writable(pair.a(), recorder.callback(&Recorder::one));
  reactor.run_once(milliseconds(100));
  EXPECT_EQ(recorder.log.size(), 1U);
}

TEST(ReactorTest, TooManyWatchesAborts) {
  Reactor reactor({.inbox = 1, .watches = 1, .timers = 1});
  SocketPair first;
  SocketPair second;
  Recorder recorder;
  reactor.when_readable(first.a(), recorder.callback(&Recorder::one));
  // Same fd: same slot.
  reactor.when_writable(first.a(), recorder.callback(&Recorder::two));
  EXPECT_DEATH(
      reactor.when_readable(second.a(), recorder.callback(&Recorder::one)),
      "");
}

TEST(ReactorTest, ForgottenFdDoesNotFire) {
  Reactor reactor(kCapacity);
  SocketPair pair;
  Recorder recorder;
  reactor.when_readable(pair.a(), recorder.callback(&Recorder::one));
  reactor.forget(pair.a());
  ASSERT_EQ(write(pair.b(), "x", 1), 1);
  reactor.run_once(milliseconds(10));
  EXPECT_TRUE(recorder.log.empty());
}

TEST(ReactorTest, CallbackMayForgetAnotherReadyFd) {
  // Whichever runs first forgets the other.
  struct Forgetter {
    Reactor& reactor;
    int other_fd;
    int* fires;
    void run() {
      ++*fires;
      reactor.forget(other_fd);
    }
  };
  Reactor reactor(kCapacity);
  SocketPair first;
  SocketPair second;
  int fires = 0;
  Forgetter forget_second{reactor, second.a(), &fires};
  Forgetter forget_first{reactor, first.a(), &fires};
  reactor.when_readable(first.a(), {&forget_second, &Forgetter::run});
  reactor.when_readable(second.a(), {&forget_first, &Forgetter::run});
  ASSERT_EQ(write(first.b(), "x", 1), 1);
  ASSERT_EQ(write(second.b(), "x", 1), 1);
  reactor.run_once(milliseconds(100));
  EXPECT_EQ(fires, 1);
}

TEST(ReactorTest, ClosedButWatchedFdIsDropped) {
  Reactor reactor(kCapacity);
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Recorder recorder;
  reactor.when_readable(fds[0], recorder.callback(&Recorder::one));
  close(fds[0]);  // Bug in the owner: closed without forget().
  close(fds[1]);
  reactor.run_once(milliseconds(10));  // select() fails with EBADF.
  reactor.post(recorder.callback(&Recorder::two));
  reactor.run_once(milliseconds(10));  // The loop keeps working.
  EXPECT_EQ(recorder.log, (std::vector<int>{2}));
}

}  // namespace
}  // namespace coprocessor
