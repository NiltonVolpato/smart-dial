// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "async/callback.h"
#include "async/handler.h"
#include "async/reactor.h"

namespace coprocessor {
namespace {

using std::chrono::milliseconds;

constexpr Reactor::Capacity kCapacity{.inbox = 8, .watches = 4, .timers = 4};

// What a Probe does in start(): wait on something, or finish at once.
enum class Wait { Nothing, Timer, Fd, Inbox, Everything };

// Logs its lifecycle into `log`, prefixed with its name.
class Probe final : public BaseHandler {
 public:
  Probe(Reactor& reactor, tether::CallId call, std::vector<std::string>* log,
        Wait wait, int fd = -1)
      : BaseHandler(reactor, call), log_(log), wait_(wait), fd_(fd) {}
  ~Probe() override { log_->push_back("destroyed"); }

 private:
  void start() override {
    log_->push_back("started");
    switch (wait_) {
      case Wait::Nothing:
        finish();
        return;
      case Wait::Timer:
        reactor().after(milliseconds(10), Callback(this, &Probe::woke));
        return;
      case Wait::Fd:
        reactor().when_readable(fd_, Callback(this, &Probe::woke));
        return;
      case Wait::Inbox:
        reactor().post(Callback(this, &Probe::woke));
        return;
      case Wait::Everything:
        reactor().after(milliseconds(10), Callback(this, &Probe::woke));
        reactor().when_readable(fd_, Callback(this, &Probe::woke));
        reactor().post(Callback(this, &Probe::woke));
        return;
    }
  }

  void woke() {
    log_->push_back("woke");
    finish();
  }

  std::vector<std::string>* log_;
  Wait wait_;
  int fd_;
};

constexpr auto kCall = tether::CallId{7};

class HandlerTest : public testing::Test {
 protected:
  HandlerTest() { EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0); }
  ~HandlerTest() override {
    close(fds_[0]);
    close(fds_[1]);
  }

  void run_for(milliseconds duration) {
    auto deadline = Reactor::Clock::now() + duration;
    while (Reactor::Clock::now() < deadline) {
      reactor_.run_once(milliseconds(5));
    }
  }

  int quiet_fd() const { return fds_[0]; }

  Reactor reactor_{kCapacity};
  std::vector<std::string> log_;
  int fds_[2] = {-1, -1};
};

TEST_F(HandlerTest, StartRunsOnTheReactor) {
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Nothing));
  EXPECT_TRUE(log_.empty());  // Handed over: nothing ran yet.
  reactor_.run_once(milliseconds(0));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, WaitsThenFinishes) {
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Timer));
  run_for(milliseconds(50));
  EXPECT_EQ(log_,
            (std::vector<std::string>{"started", "woke", "destroyed"}));
}

TEST_F(HandlerTest, CancelFindsHandlerWaitingOnTimer) {
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Timer));
  reactor_.run_once(milliseconds(0));
  reactor_.cancel(kCall);
  run_for(milliseconds(50));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, CancelFindsHandlerWaitingOnFd) {
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Fd, quiet_fd()));
  reactor_.run_once(milliseconds(0));
  reactor_.cancel(kCall);
  run_for(milliseconds(20));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, CancelFindsHandlerWaitingInInbox) {
  // Start and cancel arrive together: start() runs first and posts its next
  // step, which the cancel then finds still in the inbox.
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Inbox));
  reactor_.cancel(kCall);
  run_for(milliseconds(20));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, DestructionDropsEverythingItWaitedOn) {
  // As above: the cancel runs right after start(), while it waits on all.
  reactor_.start(
      new Probe(reactor_, kCall, &log_, Wait::Everything, quiet_fd()));
  reactor_.cancel(kCall);
  run_for(milliseconds(50));
  // No "woke": its timer, fd watch and posted step all went with it.
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, CancelOnlyTouchesItsCall) {
  std::vector<std::string> other_log;
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Timer));
  reactor_.start(
      new Probe(reactor_, tether::CallId{8}, &other_log, Wait::Timer));
  reactor_.run_once(milliseconds(0));
  reactor_.cancel(kCall);
  run_for(milliseconds(50));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
  EXPECT_EQ(other_log,
            (std::vector<std::string>{"started", "woke", "destroyed"}));
}

TEST_F(HandlerTest, CancelAfterFinishIsNoop) {
  reactor_.start(new Probe(reactor_, kCall, &log_, Wait::Nothing));
  reactor_.cancel(kCall);
  run_for(milliseconds(10));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, CallessHandlerIsNeverCancelled) {
  reactor_.start(new Probe(reactor_, tether::CallId{}, &log_, Wait::Timer));
  reactor_.run_once(milliseconds(0));
  reactor_.cancel(tether::CallId{});
  run_for(milliseconds(50));
  EXPECT_EQ(log_,
            (std::vector<std::string>{"started", "woke", "destroyed"}));
}

}  // namespace
}  // namespace coprocessor
