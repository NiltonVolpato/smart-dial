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
enum class Wait { Nothing, Timer, Fd, Inbox };

// Logs its lifecycle into `log`, prefixed with its name.
class Probe final : public BaseHandler {
 public:
  Probe(Reactor& reactor, std::vector<std::string>* log, Wait wait,
        int fd = -1)
      : BaseHandler(reactor), log_(log), wait_(wait), fd_(fd) {}
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


  Reactor reactor_{kCapacity};
  std::vector<std::string> log_;
  int fds_[2] = {-1, -1};
};

TEST_F(HandlerTest, StartRunsOnTheReactor) {
  reactor_.start(new Probe(reactor_, &log_, Wait::Nothing));
  EXPECT_TRUE(log_.empty());  // Handed over: nothing ran yet.
  reactor_.run_once(milliseconds(0));
  EXPECT_EQ(log_, (std::vector<std::string>{"started", "destroyed"}));
}

TEST_F(HandlerTest, WaitsOnTimerThenFinishes) {
  reactor_.start(new Probe(reactor_, &log_, Wait::Timer));
  run_for(milliseconds(50));
  EXPECT_EQ(log_,
            (std::vector<std::string>{"started", "woke", "destroyed"}));
}

TEST_F(HandlerTest, WaitsOnFdThenFinishes) {
  reactor_.start(new Probe(reactor_, &log_, Wait::Fd, fds_[0]));
  reactor_.run_once(milliseconds(0));
  EXPECT_EQ(log_, (std::vector<std::string>{"started"}));
  ASSERT_EQ(write(fds_[1], "x", 1), 1);
  run_for(milliseconds(20));
  EXPECT_EQ(log_,
            (std::vector<std::string>{"started", "woke", "destroyed"}));
}

TEST_F(HandlerTest, WaitsOnItsOwnPostThenFinishes) {
  reactor_.start(new Probe(reactor_, &log_, Wait::Inbox));
  reactor_.run_once(milliseconds(0));
  EXPECT_EQ(log_, (std::vector<std::string>{"started"}));  // Next iteration.
  reactor_.run_once(milliseconds(0));
  EXPECT_EQ(log_,
            (std::vector<std::string>{"started", "woke", "destroyed"}));
}

}  // namespace
}  // namespace coprocessor
