// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "async/callback.h"
#include "async/reactor.h"
#include "net/http_client.h"
#include "net/http_request.h"

namespace coprocessor {
namespace {

using std::chrono::milliseconds;

constexpr Reactor::Capacity kCapacity{.inbox = 8, .watches = 4, .timers = 4};

// Accepts one connection, reads the request head, then plays `script`: each
// step writes its bytes, then sleeps. Closes the connection at the end.
class ScriptedServer {
 public:
  struct Step {
    std::string bytes;
    milliseconds pause{0};
  };

  explicit ScriptedServer(std::vector<Step> script) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)),
              0);
    EXPECT_EQ(listen(listen_fd_, 1), 0);
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);

    thread_ = std::thread([this, script = std::move(script)] {
      int fd = accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        return;
      }
      char buf[512];
      while (received_.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
          break;
        }
        received_.append(buf, static_cast<size_t>(n));
      }
      for (const Step& step : script) {
        (void)::send(fd, step.bytes.data(), step.bytes.size(), 0);
        std::this_thread::sleep_for(step.pause);
      }
      close(fd);
    });
  }

  ~ScriptedServer() {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    thread_.join();
  }

  Endpoint endpoint() const {
    return {.ip = Ipv4{INADDR_LOOPBACK}, .port = port_};
  }
  // Valid once the exchange is done.
  const std::string& received() const { return received_; }

 private:
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  std::string received_;
};

// Enough permits for every test but the ones about permits.
ConnectionPermits& plenty_of_permits() {
  static ConnectionPermits permits(8, 8);
  return permits;
}

// Owns a client and records what it reports.
class Exchange final : public HttpResponseSink {
 public:
  explicit Exchange(Reactor& reactor, bool abort_on_body = false,
                    HttpClient::Timeouts timeouts = {},
                    ConnectionPermits& permits = plenty_of_permits())
      : client_(reactor, permits, timeouts), abort_on_body_(abort_on_body) {}

  [[nodiscard]] bool finished() const { return result.has_value(); }

  void send(const Endpoint& endpoint, std::string_view request) {
    client_.send(endpoint, request, Callback(this, &Exchange::done), this);
  }

  std::optional<HttpResult> result;
  std::string body;
  int last_calls = 0;
  std::vector<std::pair<std::string, std::string>> headers;

 private:
  void done() { result = client_.result(); }

  void on_header(std::string_view name, std::string_view value) override {
    headers.emplace_back(name, value);
  }
  bool on_body(std::string_view chunk, bool last) override {
    body.append(chunk);
    last_calls += last ? 1 : 0;
    return !abort_on_body_;
  }

  HttpClient client_;
  bool abort_on_body_;
};

void run_until_done(Reactor& reactor, const Exchange& exchange) {
  auto deadline = Reactor::Clock::now() + milliseconds(2000);
  while (!exchange.result && Reactor::Clock::now() < deadline) {
    reactor.run_once(milliseconds(50));
  }
}

constexpr std::string_view kRequest =
    "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";

TEST(HttpClientTest, ContentLengthBodyArrivesInPieces) {
  ScriptedServer server({
      {.bytes = "HTTP/1.1 200 OK\r\nContent-Le", .pause = milliseconds(10)},
      {.bytes = "ngth: 11\r\nSID: uuid:42\r\n\r\nhello", .pause = milliseconds(10)},
      {.bytes = " world"},
  });
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_TRUE(exchange.result->ok());
  EXPECT_EQ(exchange.result->status, 200);
  EXPECT_EQ(exchange.body, "hello world");
  EXPECT_EQ(exchange.last_calls, 1);
  EXPECT_EQ(server.received(), kRequest);
  ASSERT_EQ(exchange.headers.size(), 2U);
  EXPECT_EQ(exchange.headers[1],
            (std::pair<std::string, std::string>{"SID", "uuid:42"}));
}

TEST(HttpClientTest, CloseDelimitedBody) {
  ScriptedServer server({{.bytes = "HTTP/1.1 200 OK\r\n\r\n<a/>"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_TRUE(exchange.result->ok());
  EXPECT_EQ(exchange.body, "<a/>");
  EXPECT_EQ(exchange.last_calls, 1);
}

TEST(HttpClientTest, EmptyBody) {
  ScriptedServer server({{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
                          .pause = milliseconds(200)}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_TRUE(exchange.result->ok());
  EXPECT_EQ(exchange.last_calls, 1);  // Completes without waiting for close.
}

TEST(HttpClientTest, ErrorStatusSkipsBody) {
  ScriptedServer server({{.bytes =
                              "HTTP/1.1 500 Internal Server Error\r\n"
                              "Content-Length: 5\r\n\r\nfault"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_FALSE(exchange.result->ok());
  EXPECT_EQ(exchange.result->error, HttpError::None);
  EXPECT_EQ(exchange.result->status, 500);
  EXPECT_EQ(exchange.body, "");
}

TEST(HttpClientTest, TruncatedBodyIsProtocolError) {
  ScriptedServer server(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Protocol);
}

TEST(HttpClientTest, ChunkedBodyIsDecoded) {
  // Sizes in hex, an extension, and lines split across reads.
  ScriptedServer server({
      {.bytes = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r",
       .pause = milliseconds(10)},
      {.bytes = "\nhello\r\nB;ext=1\r\n world, h", .pause = milliseconds(10)},
      {.bytes = "i!\r\n0\r\n\r\n"},
  });
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_TRUE(exchange.result->ok());
  EXPECT_EQ(exchange.body, "hello world, hi!");
  EXPECT_EQ(exchange.last_calls, 1);
}

TEST(HttpClientTest, TruncatedChunkedBodyIsProtocolError) {
  ScriptedServer server({{.bytes =
                              "HTTP/1.1 200 OK\r\n"
                              "Transfer-Encoding: chunked\r\n\r\n5\r\nhello"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Protocol);
}

TEST(HttpClientTest, MalformedChunkSizeIsProtocolError) {
  ScriptedServer server({{.bytes =
                              "HTTP/1.1 200 OK\r\n"
                              "Transfer-Encoding: chunked\r\n\r\nzz\r\n"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Protocol);
}

TEST(HttpClientTest, UnknownTransferEncodingIsRejected) {
  ScriptedServer server({{.bytes =
                              "HTTP/1.1 200 OK\r\n"
                              "Transfer-Encoding: gzip\r\n\r\nxx"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Protocol);
}

// Pauses on the first piece of body, and resumes a while later: the rest
// arrives then, and the idle timeout doesn't fire meanwhile.
class PausingExchange final : public HttpResponseSink {
 public:
  explicit PausingExchange(Reactor& reactor)
      : reactor_(reactor),
        client_(reactor, plenty_of_permits(), {.idle = milliseconds(100)}) {}

  void send(const Endpoint& endpoint, std::string_view request) {
    client_.send(endpoint, request, Callback(this, &PausingExchange::done),
                 this);
  }

  std::optional<HttpResult> result;
  std::string body;
  int calls_while_paused = 0;

 private:
  void done() { result = client_.result(); }

  bool on_body(std::string_view chunk, bool /*last*/) override {
    body.append(chunk);
    calls_while_paused += paused_ ? 1 : 0;
    if (!paused_once_) {
      paused_once_ = true;
      paused_ = true;
      client_.pause();
      timer_ = reactor_.after(milliseconds(300),
                              Callback(this, &PausingExchange::resume));
    }
    return true;
  }

  void resume() {
    paused_ = false;
    client_.resume();
  }

  Reactor& reactor_;
  HttpClient client_;
  bool paused_once_ = false;
  bool paused_ = false;
  Reactor::TimerId timer_ = 0;
};

TEST(HttpClientTest, PauseHoldsTheBodyBack) {
  ScriptedServer server({
      {.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello",
       .pause = milliseconds(20)},
      {.bytes = " world"},
  });
  Reactor reactor(kCapacity);
  PausingExchange exchange(reactor);
  exchange.send(server.endpoint(), kRequest);
  auto deadline = Reactor::Clock::now() + milliseconds(2000);
  while (!exchange.result && Reactor::Clock::now() < deadline) {
    reactor.run_once(milliseconds(50));
  }

  ASSERT_TRUE(exchange.result);
  EXPECT_TRUE(exchange.result->ok());  // No idle timeout while paused.
  EXPECT_EQ(exchange.body, "hello world");
  EXPECT_EQ(exchange.calls_while_paused, 0);
}

TEST(HttpClientTest, OnBodyCanAbort) {
  ScriptedServer server(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody"}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor, /*abort_on_body=*/true);
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Aborted);
}

TEST(HttpClientTest, SilentServerTimesOut) {
  ScriptedServer server({{.pause = milliseconds(300)}});
  Reactor reactor(kCapacity);
  Exchange exchange(reactor, false,
                    {.connect = milliseconds(100), .idle = milliseconds(100)});
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Timeout);
}

TEST(HttpClientTest, ProgressExtendsIdleDeadline) {
  // Each pause is under the idle timeout, but together they're well over it.
  ScriptedServer server({
      {.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n",
       .pause = milliseconds(60)},
      {.bytes = "a", .pause = milliseconds(60)},
      {.bytes = "b", .pause = milliseconds(60)},
      {.bytes = "c"},
  });
  Reactor reactor(kCapacity);
  Exchange exchange(reactor, false,
                    {.connect = milliseconds(100), .idle = milliseconds(100)});
  exchange.send(server.endpoint(), kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_TRUE(exchange.result->ok());
  EXPECT_EQ(exchange.body, "abc");
}

TEST(HttpClientTest, RefusedConnection) {
  Endpoint endpoint;
  {
    ScriptedServer closed({});  // Bind a port, then release it.
    endpoint = closed.endpoint();
    int fd = socket(AF_INET, SOCK_STREAM, 0);  // Unblock its accept().
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
  }
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send(endpoint, kRequest);
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::Connect);
}

TEST(HttpClientTest, MissingAddressCompletesAsynchronously) {
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send({.ip = Ipv4{}, .port = 80}, kRequest);
  EXPECT_FALSE(exchange.result);  // Never from inside send().
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::BadAddress);
}

// Owns its client through a pointer, so a test can destroy the client alone.
class ClientOwner {
 public:
  explicit ClientOwner(Reactor& reactor,
                       ConnectionPermits& permits = plenty_of_permits())
      : client(std::make_unique<HttpClient>(reactor, permits)) {}

  void send(const Endpoint& endpoint, bool destroy_in_done) {
    destroy_in_done_ = destroy_in_done;
    client->send(endpoint, kRequest, Callback(this, &ClientOwner::done));
  }

  std::unique_ptr<HttpClient> client;
  bool done_ran = false;

 private:
  void done() {
    done_ran = true;
    if (destroy_in_done_) {
      client.reset();
    }
  }

  bool destroy_in_done_ = false;
};

TEST(HttpClientTest, DoneMayDestroyClient) {
  ScriptedServer server(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}});
  Reactor reactor(kCapacity);
  ClientOwner owner(reactor);
  owner.send(server.endpoint(), /*destroy_in_done=*/true);
  auto deadline = Reactor::Clock::now() + milliseconds(2000);
  while (!owner.done_ran && Reactor::Clock::now() < deadline) {
    reactor.run_once(milliseconds(50));
  }
  EXPECT_TRUE(owner.done_ran);
  EXPECT_EQ(owner.client, nullptr);
}

TEST(HttpClientTest, DestroyingClientAbortsSilently) {
  ScriptedServer server({{.pause = milliseconds(200)}});
  Reactor reactor(kCapacity);
  ClientOwner owner(reactor);
  owner.send(server.endpoint(), false);
  reactor.run_once(milliseconds(50));
  owner.client.reset();
  reactor.run_once(milliseconds(50));  // Nothing of the client fires.
  EXPECT_FALSE(owner.done_ran);
}

// One permit, two exchanges: the second waits for the first to finish.
TEST(ConnectionPermitsTest, SecondExchangeWaitsForThePermit) {
  ScriptedServer first({{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n",
                         .pause = milliseconds(100)},
                        {.bytes = "a"}});
  ScriptedServer second(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nb"}});
  ConnectionPermits permits(1, 2);
  Reactor reactor(kCapacity);
  Exchange one(reactor, false, {}, permits);
  Exchange two(reactor, false, {}, permits);
  one.send(first.endpoint(), kRequest);
  two.send(second.endpoint(), kRequest);

  // While the first is still receiving, the second hasn't even connected.
  auto deadline = Reactor::Clock::now() + milliseconds(50);
  while (Reactor::Clock::now() < deadline) {
    reactor.run_once(milliseconds(10));
  }
  EXPECT_FALSE(one.finished());
  EXPECT_FALSE(two.finished());

  run_until_done(reactor, one);
  run_until_done(reactor, two);
  ASSERT_TRUE(one.finished() && two.finished());
  EXPECT_EQ(one.body, "a");
  EXPECT_EQ(two.body, "b");
}

TEST(ConnectionPermitsTest, DestroyedHolderReleasesItsPermit) {
  ScriptedServer silent({{.pause = milliseconds(300)}});
  ScriptedServer second(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nb"}});
  ConnectionPermits permits(1, 2);
  Reactor reactor(kCapacity);
  ClientOwner holder(reactor, permits);
  Exchange waiter(reactor, false, {}, permits);
  holder.send(silent.endpoint(), false);
  waiter.send(second.endpoint(), kRequest);
  reactor.run_once(milliseconds(10));

  holder.client.reset();  // Mid-exchange: its permit passes to the waiter.
  run_until_done(reactor, waiter);
  ASSERT_TRUE(waiter.finished());
  EXPECT_EQ(waiter.body, "b");
}

TEST(ConnectionPermitsTest, DestroyedWaiterLeavesTheLine) {
  ScriptedServer first(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na",
        .pause = milliseconds(50)}});
  ScriptedServer third(
      {{.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nc"}});
  ConnectionPermits permits(1, 2);
  Reactor reactor(kCapacity);
  Exchange one(reactor, false, {}, permits);
  auto two = std::make_unique<ClientOwner>(reactor, permits);
  Exchange three(reactor, false, {}, permits);
  one.send(first.endpoint(), kRequest);
  two->send(first.endpoint(), false);  // Waits; never connects.
  three.send(third.endpoint(), kRequest);
  two.reset();  // Leaves the line: the permit skips it.

  run_until_done(reactor, one);
  run_until_done(reactor, three);
  EXPECT_EQ(three.body, "c");
}

TEST(FixedRequestTest, FormatsInPlace) {
  FixedRequest<64> request;
  // `\n` becomes `\r\n`.
  request.append("GET {} HTTP/1.1\n", "/path");
  request.append("Content-Length: {}\n\n", 0);
  EXPECT_TRUE(request.fits());
  EXPECT_EQ(std::string_view(request.bytes().data(), request.bytes().size()),
            "GET /path HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
}

TEST(FixedRequestTest, ReportsOverflow) {
  FixedRequest<8> request;
  request.append("{}", "0123456789");
  EXPECT_FALSE(request.fits());
  EXPECT_EQ(request.bytes().size(), 8U);
}

}  // namespace
}  // namespace coprocessor
