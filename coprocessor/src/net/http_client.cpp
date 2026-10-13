// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "net/http_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "heap_log.h"

#include "log.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "http";

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket.
#endif

bool would_block() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

std::string_view trim(std::string_view s) {
  constexpr std::string_view kSpace = " \t";
  size_t first = s.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

// A chunk larger than this is malformed, or more than we'd take anyway.
constexpr std::size_t kMaxChunkSize = std::size_t{1} << 24;

// 0..15, or -1 if `c` isn't a hex digit.
int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool iequals(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    return lower(x) == lower(y);
  });
}

template <class T>
bool parse_number(std::string_view s, T* out) {
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
  return ec == std::errc{} && ptr == s.data() + s.size();
}

}  // namespace

HttpClient::HttpClient(Reactor& reactor, ConnectionPermits& permits,
                       Timeouts timeouts)
    : reactor_(reactor), permits_(permits), timeouts_(timeouts) {}

HttpClient::~HttpClient() { release(); }

void HttpClient::send(const Endpoint& endpoint, std::span<const char> request,
                      tether::Callback done, HttpResponseSink* sink) {
  if (done_) {
    ESP_LOGE(TAG, "send() while an exchange is in progress");
    std::abort();
  }
  done_ = done;
  sink_ = sink;
  result_ = {};
  request_ = request;
  sent_ = 0;
  status_ = 0;
  content_length_.reset();
  body_received_ = 0;
  buffered_ = 0;
  paused_ = false;
  read_pending_ = false;
  chunked_ = Chunked::No;
  chunk_left_ = 0;
  chunk_size_seen_ = false;

  if (endpoint.ip == Ipv4{}) {
    fail_soon(HttpError::BadAddress);
    return;
  }
  endpoint_ = endpoint;
  if (permits_.acquire(this)) {
    has_permit_ = true;
    open_connection();
  } else {
    waiting_ = true;  // Until granted().
  }
}

void HttpClient::granted() {
  waiting_ = false;
  has_permit_ = true;
  timer_ = reactor_.after(std::chrono::milliseconds(0),
                          callback(&HttpClient::on_granted));
}

void HttpClient::on_granted() {
  timer_ = 0;
  open_connection();
}

void HttpClient::open_connection() {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(endpoint_.port);
  addr.sin_addr = endpoint_.ip.to_in_addr();

  fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd_ < 0) {
    fail_soon(HttpError::Io);
    return;
  }
  fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

  // Even an immediate connect goes through the reactor, so that on_done never
  // runs inside send().
  if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
      errno != EINPROGRESS) {
    fail_soon(HttpError::Connect);
    return;
  }
  extend_deadline(timeouts_.connect);
  reactor_.when_writable(fd_, callback(&HttpClient::on_connected));
}

void HttpClient::on_connected() {
  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
    complete({.error = HttpError::Connect});
    return;
  }
  extend_deadline(timeouts_.idle);
  send_some();
}

void HttpClient::send_some() {
  ssize_t n =
      ::send(fd_, request_.data() + sent_, request_.size() - sent_, kSendFlags);
  if (n < 0) {
    if (would_block()) {
      reactor_.when_writable(fd_, callback(&HttpClient::send_some));
      return;
    }
    complete({.error = HttpError::Io});
    return;
  }
  sent_ += static_cast<std::size_t>(n);
  extend_deadline(timeouts_.idle);
  if (sent_ < request_.size()) {
    reactor_.when_writable(fd_, callback(&HttpClient::send_some));
    return;
  }
  reactor_.when_readable(fd_, callback(&HttpClient::receive_head));
}

void HttpClient::receive_head() {
  ssize_t n =
      recv(fd_, buffer_.data() + buffered_, buffer_.size() - buffered_, 0);
  if (n < 0) {
    if (would_block()) {
      reactor_.when_readable(fd_, callback(&HttpClient::receive_head));
      return;
    }
    complete({.error = HttpError::Io});
    return;
  }
  if (n == 0) {
    complete({.error = HttpError::Protocol});  // Closed before the head.
    return;
  }
  // The terminator may straddle the previous read.
  std::size_t search_from = buffered_ >= 3 ? buffered_ - 3 : 0;
  buffered_ += static_cast<std::size_t>(n);
  extend_deadline(timeouts_.idle);

  std::string_view data(buffer_.data(), buffered_);
  std::size_t head_end = data.find("\r\n\r\n", search_from);
  if (head_end == std::string_view::npos) {
    if (buffered_ == buffer_.size()) {
      complete({.error = HttpError::Protocol});  // Head too large.
      return;
    }
    reactor_.when_readable(fd_, callback(&HttpClient::receive_head));
    return;
  }
  if (!parse_head(head_end)) {
    return;
  }
  if (status_ < 200 || status_ >= 300) {
    // The body of an error isn't delivered: done.
    complete({.status = status_});
    return;
  }

  std::size_t body_start = head_end + 4;
  std::string_view first(buffer_.data() + body_start, buffered_ - body_start);
  // An empty body completes here; otherwise deliver what came with the head.
  if (!first.empty() || content_length_ == 0) {
    if (!deliver(first, false)) {
      return;
    }
  }
  read_body();
}

void HttpClient::read_body() {
  if (paused_) {
    read_pending_ = true;
    return;
  }
  reactor_.when_readable(fd_, callback(&HttpClient::receive_body));
}

void HttpClient::pause() { paused_ = true; }

void HttpClient::resume() {
  paused_ = false;
  if (fd_ < 0 || !std::exchange(read_pending_, false)) {
    return;
  }
  extend_deadline(timeouts_.idle);
  reactor_.when_readable(fd_, callback(&HttpClient::receive_body));
}

bool HttpClient::parse_head(std::size_t head_size) {
  std::string_view head(buffer_.data(), head_size);
  std::size_t line_end = head.find("\r\n");
  std::string_view status_line = head.substr(0, line_end);
  std::size_t space = status_line.find(' ');
  if (!status_line.starts_with("HTTP/1.") || space == std::string_view::npos ||
      !parse_number(status_line.substr(space + 1, 3), &status_)) {
    complete({.error = HttpError::Protocol});
    return false;
  }

  std::string_view rest = line_end == std::string_view::npos
                              ? std::string_view{}
                              : head.substr(line_end + 2);
  while (!rest.empty()) {
    std::size_t end = rest.find("\r\n");
    std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 2);
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    std::string_view name = trim(line.substr(0, colon));
    std::string_view value = trim(line.substr(colon + 1));
    if (iequals(name, "Content-Length")) {
      std::size_t length = 0;
      if (!parse_number(value, &length)) {
        complete({.error = HttpError::Protocol, .status = status_});
        return false;
      }
      content_length_ = length;
    } else if (iequals(name, "Transfer-Encoding")) {
      if (iequals(value, "chunked")) {
        chunked_ = Chunked::Size;
      } else if (!iequals(value, "identity")) {
        complete({.error = HttpError::Protocol, .status = status_});
        return false;  // E.g. gzip: undecodable here.
      }
    }
    if (sink_ != nullptr) {
      sink_->on_header(name, value);
    }
  }
  return true;
}

void HttpClient::receive_body() {
  ssize_t n = recv(fd_, buffer_.data(), buffer_.size(), 0);
  if (n < 0) {
    if (would_block()) {
      reactor_.when_readable(fd_, callback(&HttpClient::receive_body));
      return;
    }
    complete({.error = HttpError::Io, .status = status_});
    return;
  }
  if (n == 0) {
    if (content_length_) {
      // Reaching Content-Length completes, so this is a truncated body.
      complete({.error = HttpError::Protocol, .status = status_});
      return;
    }
    deliver({}, true);
    return;
  }
  extend_deadline(timeouts_.idle);
  if (deliver({buffer_.data(), static_cast<std::size_t>(n)}, false)) {
    read_body();
  }
}

bool HttpClient::deliver(std::string_view chunk, bool eof) {
  if (chunked_ != Chunked::No) {
    return deliver_chunked(chunk, eof);
  }
  if (content_length_) {
    // Ignore anything past Content-Length.
    chunk = chunk.substr(0, *content_length_ - body_received_);
  }
  body_received_ += chunk.size();
  bool last = eof || body_received_ == content_length_;
  if (sink_ != nullptr && !sink_->on_body(chunk, last)) {
    complete({.error = HttpError::Aborted, .status = status_});
    return false;
  }
  if (last) {
    complete({.status = status_});
    return false;
  }
  return true;
}

// Decodes chunked transfer coding (RFC 9112, 7.1) as it arrives, handing on
// each piece of data. The last chunk ends the body: trailers aren't read,
// the connection closes anyway.
bool HttpClient::deliver_chunked(std::string_view raw, bool eof) {
  while (!raw.empty()) {
    const char c = raw.front();
    switch (chunked_) {
      case Chunked::Size:
        if (const int digit = hex_digit(c); digit >= 0) {
          if (chunk_left_ > (kMaxChunkSize >> 4)) {
            complete({.error = HttpError::Protocol, .status = status_});
            return false;
          }
          chunk_left_ = chunk_left_ * 16 + static_cast<std::size_t>(digit);
          chunk_size_seen_ = true;
        } else if (c == ';' || c == ' ' || c == '\t') {
          chunked_ = Chunked::Extension;
        } else if (c == '\n') {
          if (!size_line_ended()) {
            return false;
          }
        } else if (c != '\r') {
          complete({.error = HttpError::Protocol, .status = status_});
          return false;
        }
        raw.remove_prefix(1);
        break;
      case Chunked::Extension:  // Skipped: none is meaningful to us.
        if (c == '\n' && !size_line_ended()) {
          return false;
        }
        raw.remove_prefix(1);
        break;
      case Chunked::Data: {
        const std::string_view piece = raw.substr(0, chunk_left_);
        raw.remove_prefix(piece.size());
        chunk_left_ -= piece.size();
        body_received_ += piece.size();
        if (chunk_left_ == 0) {
          chunked_ = Chunked::DataEnd;
        }
        if (sink_ != nullptr && !sink_->on_body(piece, false)) {
          complete({.error = HttpError::Aborted, .status = status_});
          return false;
        }
        break;
      }
      case Chunked::DataEnd:  // The CRLF after a chunk's data.
        if (c == '\n') {
          chunked_ = Chunked::Size;
        } else if (c != '\r') {
          complete({.error = HttpError::Protocol, .status = status_});
          return false;
        }
        raw.remove_prefix(1);
        break;
      case Chunked::No:
      case Chunked::Done:
        return false;  // Unreachable: Done completed the exchange.
    }
  }
  if (eof) {
    complete({.error = HttpError::Protocol, .status = status_});  // Truncated.
    return false;
  }
  return true;
}

// A size line ended: its chunk's data follows, or, at size 0, the body
// ended. False if that completed the exchange.
bool HttpClient::size_line_ended() {
  if (!chunk_size_seen_) {
    complete({.error = HttpError::Protocol, .status = status_});
    return false;
  }
  chunk_size_seen_ = false;
  if (chunk_left_ > 0) {
    chunked_ = Chunked::Data;
    return true;
  }
  chunked_ = Chunked::Done;
  if (sink_ != nullptr && !sink_->on_body({}, true)) {
    complete({.error = HttpError::Aborted, .status = status_});
    return false;
  }
  complete({.status = status_});
  return false;
}

void HttpClient::extend_deadline(std::chrono::milliseconds timeout) {
  deadline_ = Reactor::Clock::now() + timeout;
  if (timer_ != 0) {
    if (timer_at_ <= deadline_) {
      return;  // It fires first, and re-arms for the rest.
    }
    reactor_.cancel_timer(timer_);  // A shorter timeout: fire earlier.
  }
  timer_ = reactor_.after(timeout, callback(&HttpClient::check_deadline));
  timer_at_ = deadline_;
}

void HttpClient::check_deadline() {
  timer_ = 0;
  const Reactor::Clock::time_point now = Reactor::Clock::now();
  if (paused_) {
    // The sink holds the reading back: the peer isn't idle.
    extend_deadline(timeouts_.idle);
    return;
  }
  if (now < deadline_) {
    timer_ =
        reactor_.after(deadline_ - now, callback(&HttpClient::check_deadline));
    timer_at_ = deadline_;
    return;
  }
  complete({.error = HttpError::Timeout, .status = status_});
}

void HttpClient::fail_soon(HttpError error) {
  // Through a timer, so that destroying the client cancels it.
  pending_error_ = error;
  timer_ = reactor_.after(std::chrono::milliseconds(0),
                          callback(&HttpClient::fail_pending));
}

void HttpClient::fail_pending() {
  timer_ = 0;
  complete({.error = pending_error_});
}

void HttpClient::complete(HttpResult result) {
  release();
  log_heap("HTTP exchange done");
  result_ = result;
  sink_ = nullptr;
  tether::Callback done = std::exchange(done_, tether::Callback{});
  done();  // Last: it may destroy this client.
}

void HttpClient::release() {
  if (timer_ != 0) {
    reactor_.cancel_timer(timer_);
    timer_ = 0;
  }
  if (fd_ >= 0) {
    reactor_.forget(fd_);
    close(fd_);
    fd_ = -1;
  }
  if (waiting_) {
    permits_.leave(this);
    waiting_ = false;
  }
  if (has_permit_) {
    has_permit_ = false;
    permits_.release();  // After closing: the next in line may open one.
  }
}

}  // namespace coprocessor
