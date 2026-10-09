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
#include "async/handler.h"
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

  Endpoint endpoint() const { return {.ip = "127.0.0.1", .port = port_}; }
  // Valid once the exchange is done.
  const std::string& received() const { return received_; }

 private:
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  std::string received_;
};

// Owns a client and records what it reports. Never started: a stack-allocated
// handler is enough to own the client's callbacks.
class Exchange final : public BaseHandler, public HttpResponseSink {
 public:
  explicit Exchange(Reactor& reactor, bool abort_on_body = false,
                    HttpClient::Timeouts timeouts = {})
      : BaseHandler(reactor, tether::CallId{}),
        client_(reactor, *this, timeouts),
        abort_on_body_(abort_on_body) {}

  void send(const Endpoint& endpoint, std::string_view request) {
    client_.send(endpoint, request, Callback(this, &Exchange::done), this);
  }

  std::optional<HttpResult> result;
  std::string body;
  int last_calls = 0;
  std::vector<std::pair<std::string, std::string>> headers;

 private:
  void start() override {}
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

TEST(HttpClientTest, ChunkedEncodingIsRejected) {
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

TEST(HttpClientTest, BadAddressCompletesAsynchronously) {
  Reactor reactor(kCapacity);
  Exchange exchange(reactor);
  exchange.send({.ip = "not-an-ip", .port = 80}, kRequest);
  EXPECT_FALSE(exchange.result);  // Never from inside send().
  run_until_done(reactor, exchange);

  ASSERT_TRUE(exchange.result);
  EXPECT_EQ(exchange.result->error, HttpError::BadAddress);
}

// Owns its client through a pointer, so a test can destroy the client alone.
class ClientOwner final : public BaseHandler {
 public:
  explicit ClientOwner(Reactor& reactor)
      : BaseHandler(reactor, tether::CallId{}),
        client(std::make_unique<HttpClient>(reactor, *this)) {}

  void send(const Endpoint& endpoint, bool destroy_in_done) {
    destroy_in_done_ = destroy_in_done;
    client->send(endpoint, kRequest, Callback(this, &ClientOwner::done));
  }

  std::unique_ptr<HttpClient> client;
  bool done_ran = false;

 private:
  void start() override {}
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
