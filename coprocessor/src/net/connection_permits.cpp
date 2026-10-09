// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "net/connection_permits.h"

#include <algorithm>
#include <cstdlib>

#include "log.h"
#include "net/http_client.h"

namespace coprocessor {

namespace {
constexpr const char* TAG = "permits";
}

ConnectionPermits::ConnectionPermits(std::size_t permits,
                                     std::size_t max_waiters)
    : available_(permits) {
  waiters_.reserve(max_waiters);
}

bool ConnectionPermits::acquire(HttpClient* client) {
  if (available_ > 0) {
    --available_;
    return true;
  }
  if (waiters_.size() == waiters_.capacity()) {
    ESP_LOGE(TAG, "Too many connections waiting (%u)",
             static_cast<unsigned>(waiters_.capacity()));
    std::abort();
  }
  waiters_.push_back(client);
  return false;
}

void ConnectionPermits::release() {
  if (waiters_.empty()) {
    ++available_;
    return;
  }
  HttpClient* next = waiters_.front();
  waiters_.erase(waiters_.begin());  // A handful: shifting beats a ring.
  next->granted();                   // The permit passes on.
}

void ConnectionPermits::leave(HttpClient* client) {
  std::erase(waiters_, client);
}

}  // namespace coprocessor
