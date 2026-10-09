// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "async/callback.h"
#include "async/reactor.h"
#include "net/connection_permits.h"
#include "net/ipv4.h"

namespace coprocessor {

struct Endpoint {
  Ipv4 ip;
  uint16_t port = 0;
};

enum class HttpError : uint8_t {
  None,
  BadAddress,  // The endpoint has no IP (0).
  Connect,     // Connection refused, unreachable, ...
  Timeout,     // Connecting, or no progress for too long.
  Io,          // send/recv failed.
  Protocol,    // Malformed or unsupported response (e.g. chunked encoding).
  Aborted,     // The sink's on_body asked to stop.
};

struct HttpResult {
  HttpError error = HttpError::None;
  int status = 0;  // HTTP status, once the response head arrived.

  [[nodiscard]] bool ok() const {
    return error == HttpError::None && status >= 200 && status < 300;
  }
};

/// Receives a response as it arrives, synchronously from inside the client:
/// it must not destroy the client. Strings are only valid during the call.
class HttpResponseSink {
 public:
  /// Each response header, value trimmed.
  virtual void on_header(std::string_view /*name*/,
                         std::string_view /*value*/) {}
  /// The body of a 2xx response; `last` is set on the final call, whose chunk
  /// may be empty. Return false to abort the exchange.
  virtual bool on_body(std::string_view /*chunk*/, bool /*last*/) {
    return true;
  }

 protected:
  ~HttpResponseSink() = default;
};

/// One HTTP/1.1 exchange at a time, on a fresh connection that the server
/// closes (requests say `Connection: close`), driven by the reactor. Only
/// Content-Length or close-delimited bodies are supported.
///
/// Each exchange first takes a ConnectionPermit, waiting in line if none is
/// free; its timeouts only start once it has one.
///
/// Reactor task only. Destroying the client aborts its exchange, without
/// running `done`, unregisters everything it had in the reactor, and gives
/// back its permit (or leaves the line).
class HttpClient {
 public:
  struct Timeouts {
    std::chrono::milliseconds connect{3000};
    /// Without any progress, once connected.
    std::chrono::milliseconds idle{5000};
  };

  HttpClient(Reactor& reactor, ConnectionPermits& permits)
      : HttpClient(reactor, permits, Timeouts{}) {}
  HttpClient(Reactor& reactor, ConnectionPermits& permits, Timeouts timeouts);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  /// Starts an exchange; the previous one must be done. `request` is sent as
  /// is and must stay valid until `done` runs. `done` runs once, never from
  /// inside send(), and last, so it may destroy the client or start the next
  /// exchange; it reads the outcome from result().
  void send(const Endpoint& endpoint, std::span<const char> request,
            Callback done, HttpResponseSink* sink = nullptr);

  /// The last exchange's outcome, once its `done` ran.
  [[nodiscard]] const HttpResult& result() const { return result_; }

 private:
  friend class ConnectionPermits;

  // From ConnectionPermits: our turn came. Connects from the reactor, not
  // from inside whoever released the permit.
  void granted();
  void on_granted();
  void open_connection();
  void on_connected();
  void send_some();
  void receive_head();
  void receive_body();
  // These return false if they completed the exchange: return at once.
  bool parse_head(std::size_t head_size);
  bool deliver(std::string_view chunk, bool eof);
  void extend_deadline(std::chrono::milliseconds timeout);
  void check_deadline();
  void fail_soon(HttpError error);
  void fail_pending();
  void complete(HttpResult result);
  void release();

  Callback callback(void (HttpClient::*method)()) { return {this, method}; }

  Reactor& reactor_;
  ConnectionPermits& permits_;
  Timeouts timeouts_;
  int fd_ = -1;
  // One timer for the whole exchange. Progress only moves deadline_; the
  // timer, when it fires early, re-arms for what's left.
  Reactor::TimerId timer_ = 0;
  Reactor::Clock::time_point timer_at_;  // When timer_ fires.
  Reactor::Clock::time_point deadline_;
  Callback done_;
  HttpResponseSink* sink_ = nullptr;
  HttpResult result_;
  HttpError pending_error_ = HttpError::None;  // For fail_soon().

  Endpoint endpoint_;
  bool waiting_ = false;  // In the permits' line.
  bool has_permit_ = false;
  std::span<const char> request_;
  std::size_t sent_ = 0;

  int status_ = 0;
  std::optional<std::size_t> content_length_;
  std::size_t body_received_ = 0;

  // Holds the response head while it arrives, then body chunks.
  std::array<char, 1024> buffer_{};
  std::size_t buffered_ = 0;
};

}  // namespace coprocessor
