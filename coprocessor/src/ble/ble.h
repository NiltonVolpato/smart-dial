// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_npl.h"
#include "tether/callback.h"

namespace coprocessor {

/// The BLE host (NimBLE), lent to one user at a time: Improv provisioning now,
/// app profiles later. The user, a BleProfile, owns the GATT server and
/// advertising while it holds BLE. Everything a profile does, and everything
/// done to it, runs on NimBLE's host task: post work there with ble_post().

/// What a user of BLE implements. All on the host task.
class BleProfile {
 public:
  /// The services to register, ending with a {.type = 0} entry. Their
  /// characteristics' access_cb must be ble_gatt_access. Valid while the
  /// profile holds BLE.
  [[nodiscard]] virtual const ble_gatt_svc_def* services() const = 0;
  /// The host is up and the profile holds BLE: advertise.
  virtual void on_ready() = 0;
  virtual void on_gap_event(const ble_gap_event& event) = 0;
  virtual int on_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                             ble_gatt_access_ctxt& ctxt) = 0;

 protected:
  ~BleProfile() = default;
};

/// Starts NimBLE's host task, with only the GAP and GATT services, named
/// "Smart Dial XXXX" after the MAC address.
void ble_init();

namespace detail {

struct BlePost {
  ble_npl_event event{};
  virtual ~BlePost() = default;
  virtual void run() = 0;
};
void ble_post(std::unique_ptr<BlePost> post);

}  // namespace detail

/// Runs `work` on the host task. Any task.
template <class Work>
void ble_post(Work work) {
  struct Post final : detail::BlePost {
    explicit Post(Work w) : work(std::move(w)) {}
    void run() override { work(); }
    Work work;
  };
  detail::ble_post(std::make_unique<Post>(std::move(work)));
}

/// Gives BLE to `profile`: registers its services, then calls its on_ready().
/// False if the host isn't up yet or another profile holds BLE. Host task.
bool ble_acquire(BleProfile& profile);
/// Takes BLE back: stops advertising, removes the profile's services and
/// disconnects peers. The profile may be destroyed right after. Host task.
void ble_release(BleProfile& profile);
/// Advertises `fields`, with the device name in the scan response, until a
/// peer connects. For the profile holding BLE; false on failure. Host task.
bool ble_advertise(const ble_hs_adv_fields& fields);
/// The access_cb of every profile characteristic: forwards to the profile
/// holding BLE.
int ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                    ble_gatt_access_ctxt* ctxt, void* arg);
/// The name peers see. Host task.
const char* ble_device_name();

/// A one-shot timer whose callback runs on the host task. Host task.
class BleTimer {
 public:
  explicit BleTimer(tether::Callback callback);
  ~BleTimer();
  BleTimer(const BleTimer&) = delete;
  BleTimer& operator=(const BleTimer&) = delete;

  /// (Re)starts it.
  void start(uint32_t ms);
  void stop();

 private:
  static void fire(ble_npl_event* event);

  ble_npl_callout callout_{};
  tether::Callback callback_;
};

}  // namespace coprocessor
