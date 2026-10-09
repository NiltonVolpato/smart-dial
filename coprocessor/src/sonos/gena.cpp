// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "sonos/gena.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "async/handler.h"
#include "log.h"
#include "net/network.h"

namespace coprocessor {

namespace {

constexpr const char* TAG = "gena";

// What we ask for; speakers grant up to this.
constexpr uint32_t kRequestedSeconds = 1800;

std::atomic<uint32_t> s_callback_host{0};  // An Ipv4's value; 0: ours.
std::atomic<uint16_t> s_callback_port{0};

bool iequals(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    return lower(x) == lower(y);
  });
}

// UNSUBSCRIBEs in the background, for a subscription that's gone: a handler
// with no call to answer, that finishes when the speaker has replied.
class Unsubscribe final : public BaseHandler {
 public:
  Unsubscribe(Reactor& reactor, ConnectionPermits& permits, Endpoint speaker,
              std::string_view event_path, std::string_view sid)
      : BaseHandler(reactor), speaker_(speaker), http_(reactor, permits) {
    request_.append(R"(UNSUBSCRIBE {} HTTP/1.1
HOST: {}:{}
SID: {}
Connection: close

)",
                    event_path, speaker.ip, speaker.port, sid);
  }

 private:
  void start() override {
    http_.send(speaker_, request_.bytes(), Callback(this, &Unsubscribe::done));
  }

  void done() {
    const HttpResult& result = http_.result();
    if (!result.ok()) {
      ESP_LOGW(TAG, "UNSUBSCRIBE to %s: HTTP %d (error %d)",
               speaker_.ip.to_string().c_str(), result.status,
               static_cast<int>(result.error));
    }
    finish();
  }

  Endpoint speaker_;
  HttpClient http_;
  FixedRequest<256> request_;
};

}  // namespace

GenaRegistry::GenaRegistry(std::size_t capacity) : entries_(capacity) {}

void GenaRegistry::add(std::string_view sid, NotifyListener* listener) {
  std::scoped_lock lock(mutex_);
  for (Entry& entry : entries_) {
    if (entry.listener != nullptr) {
      continue;
    }
    if (sid.size() > entry.text.size()) {
      ESP_LOGE(TAG, "SID too long (%u): its NOTIFYs will be refused",
               static_cast<unsigned>(sid.size()));
      return;
    }
    std::ranges::copy(sid, entry.text.begin());
    entry.size = sid.size();
    entry.listener = listener;
    return;
  }
  ESP_LOGE(TAG, "Too many subscriptions (%u)",
           static_cast<unsigned>(entries_.size()));
  std::abort();
}

void GenaRegistry::remove(NotifyListener* listener) {
  std::scoped_lock lock(mutex_);
  for (Entry& entry : entries_) {
    if (entry.listener == listener) {
      entry = {};
    }
  }
}

void GenaRegistry::remove(std::string_view sid) {
  std::scoped_lock lock(mutex_);
  for (Entry& entry : entries_) {
    if (entry.listener != nullptr && entry.sid() == sid) {
      entry = {};
    }
  }
}

void gena_set_callback(Ipv4 host, uint16_t port) {
  s_callback_host.store(host.value, std::memory_order_relaxed);
  s_callback_port.store(port, std::memory_order_relaxed);
  ESP_LOGI(TAG, "NOTIFYs go to %s:%u", host.to_string().c_str(), port);
}

GenaSubscription::GenaSubscription(Reactor& reactor, ConnectionPermits& permits,
                                   GenaRegistry& registry,
                                   NotifyListener& listener, Endpoint speaker,
                                   std::string_view event_path)
    : reactor_(reactor),
      permits_(permits),
      registry_(registry),
      listener_(listener),
      speaker_(speaker),
      event_path_(event_path),
      http_(reactor, permits) {}

GenaSubscription::~GenaSubscription() {
  if (subscribed()) {
    registry_.remove(sid());
    reactor_.start(
        new Unsubscribe(reactor_, permits_, speaker_, event_path_, sid()));
  }
}

void GenaSubscription::refresh(Callback done) {
  done_ = done;
  send(subscribed() ? Step::Renew : Step::Subscribe);
}

std::chrono::seconds GenaSubscription::renew_after() const {
  const uint32_t granted =
      granted_seconds_ > 0 ? granted_seconds_ : kRequestedSeconds;
  return std::chrono::seconds(std::max<uint32_t>(granted / 2, 30));
}

void GenaSubscription::send(Step step) {
  step_ = step;
  request_ = {};
  if (step == Step::Subscribe) {
    Ipv4 host{s_callback_host.load(std::memory_order_relaxed)};
    uint16_t port = s_callback_port.load(std::memory_order_relaxed);
    if (host == Ipv4{}) {
      host = network_ip();
      port = 80;
    }
    request_.append(R"(SUBSCRIBE {} HTTP/1.1
HOST: {}:{}
CALLBACK: <http://{}:{}/media/notify>
NT: upnp:event
TIMEOUT: Second-{}
Connection: close

)",
                    event_path_, speaker_.ip, speaker_.port, host, port,
                    kRequestedSeconds);
  } else {
    request_.append(R"(SUBSCRIBE {} HTTP/1.1
HOST: {}:{}
SID: {}
TIMEOUT: Second-{}
Connection: close

)",
                    event_path_, speaker_.ip, speaker_.port, sid(),
                    kRequestedSeconds);
  }
  http_.send(speaker_, request_.bytes(),
             Callback(this, &GenaSubscription::sent), this);
}

void GenaSubscription::on_header(std::string_view name,
                                 std::string_view value) {
  if (iequals(name, "SID") && step_ == Step::Subscribe && !subscribed()) {
    // Registered as soon as it's known, before the exchange completes: the
    // speaker sends its first NOTIFY right after replying, and one for an
    // unknown SID gets a 412, which ends the subscription.
    set_sid(value);
    registry_.add(sid(), &listener_);
  } else if (iequals(name, "TIMEOUT")) {
    constexpr std::string_view kPrefix = "Second-";
    if (value.size() > kPrefix.size() &&
        iequals(value.substr(0, kPrefix.size()), kPrefix)) {
      uint32_t seconds = 0;
      const std::string_view digits = value.substr(kPrefix.size());
      auto [ptr, ec] = std::from_chars(digits.data(),
                                       digits.data() + digits.size(), seconds);
      if (ec == std::errc{}) {
        granted_seconds_ = seconds;
      }
    }
  }
}

void GenaSubscription::sent() {
  const HttpResult& result = http_.result();
  if (step_ == Step::Renew && result.error == HttpError::None &&
      result.status == 412) {
    ESP_LOGW(TAG, "%s on %s forgot us: subscribing again",
             std::string(event_path_).c_str(), speaker_.ip.to_string().c_str());
    registry_.remove(sid());
    set_sid({});
    send(Step::Subscribe);
    return;
  }
  if (!result.ok()) {
    ESP_LOGW(TAG, "%s %s on %s: HTTP %d (error %d)",
             step_ == Step::Subscribe ? "SUBSCRIBE" : "RENEW",
             std::string(event_path_).c_str(), speaker_.ip.to_string().c_str(),
             result.status, static_cast<int>(result.error));
    if (step_ == Step::Subscribe && subscribed()) {
      registry_.remove(sid());  // A SID with a failed exchange: don't trust it.
      set_sid({});
    }
  } else {
    ESP_LOGI(TAG, "%s %s on %s (SID %.*s, %us)",
             step_ == Step::Subscribe ? "Subscribed to" : "Renewed",
             std::string(event_path_).c_str(), speaker_.ip.to_string().c_str(),
             static_cast<int>(sid().size()), sid().data(),
             static_cast<unsigned>(granted_seconds_));
  }
  Callback done = std::exchange(done_, Callback{});
  done();  // Last: it may destroy this subscription.
}

void GenaSubscription::set_sid(std::string_view sid) {
  sid_size_ = std::min(sid.size(), sid_.size());
  std::copy_n(sid.data(), sid_size_, sid_.begin());
}

}  // namespace coprocessor
