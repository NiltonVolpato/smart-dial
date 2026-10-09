// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <vector>

namespace coprocessor {

class HttpClient;

/// Bounds outgoing connections: they share lwIP's small socket budget with the
/// web server. An HttpClient takes a permit before opening its socket, and
/// waits in line while none is free. Reactor task only.
class ConnectionPermits {
 public:
  /// `max_waiters` bounds the line (e.g. every HttpClient that may exist);
  /// overflowing it aborts. Allocated once, here.
  ConnectionPermits(std::size_t permits, std::size_t max_waiters);
  ConnectionPermits(const ConnectionPermits&) = delete;
  ConnectionPermits& operator=(const ConnectionPermits&) = delete;

 private:
  friend class HttpClient;

  /// A permit now (true), or `client` gets in line for one (false) and is
  /// granted it later, in order, through HttpClient::granted().
  bool acquire(HttpClient* client);
  /// Returns a permit: the first in line gets it.
  void release();
  /// Leaves the line, e.g. as `client` is destroyed while waiting.
  void leave(HttpClient* client);

  std::size_t available_;
  std::vector<HttpClient*> waiters_;  // In order; never grows past reserve().
};

}  // namespace coprocessor
