// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! System state for the system menu, one stream per source. The event loop
//! keeps the `theme::SystemState` they make up, and hands it to the app shell
//! (`app_shell::AppShell::set_system_state`).
//!
//! Every end runs on Core 0's thread-mode executor, hence `NoopRawMutex`.

extern crate alloc;

use alloc::rc::Rc;

use embassy_sync::blocking_mutex::raw::NoopRawMutex;
use embassy_sync::signal::Signal;
use embassy_sync::watch::Watch;

/// Wi-Fi as the co-processor reports it.
pub struct Wifi {
    pub state: theme::WifiState,
    /// The network being joined or joined; empty if none.
    pub ssid: slint::SharedString,
}

#[derive(Clone, Copy, PartialEq)]
pub struct Battery {
    /// -1 while plugged in: the voltage can't tell then.
    pub percent: i32,
    pub is_plugged: bool,
}

pub type WifiSignal = Signal<NoopRawMutex, Wifi>;
/// Whether a provisioning session is open.
pub type ProvisioningSignal = Signal<NoopRawMutex, bool>;
/// The event loop's receiver; the screensaver reads it anonymously.
pub type BatteryWatch = Watch<NoopRawMutex, Battery, 1>;

/// What the event loop follows to keep the system menu current.
pub struct SystemSources {
    pub wifi: Rc<WifiSignal>,
    pub provisioning: Rc<ProvisioningSignal>,
    pub battery: Rc<BatteryWatch>,
}

impl Default for SystemSources {
    fn default() -> Self {
        Self {
            wifi: Rc::new(Signal::new()),
            provisioning: Rc::new(Signal::new()),
            battery: Rc::new(Watch::new()),
        }
    }
}
