// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Platform abstractions and concrete host implementations.

use theme::{AppInfo, SystemMenuAction};

use crate::feedback::Feedback;

#[cfg(feature = "std")]
mod std_platform;

#[cfg(feature = "std")]
pub use std_platform::StdPlatform;

/// Platform-specific integration hooks and hardware facilities.
pub trait Platform {
    /// Called immediately after an application instance has been launched.
    fn on_app_launched(&self, info: &AppInfo);

    /// Wakes the platform event loop to process state updates and render dirty regions.
    fn wake_event_loop(&self);

    /// Current wall-clock time as a UTC `jiff::Timestamp`, if available on this platform.
    fn now_utc(&self) -> Option<jiff::Timestamp>;

    /// Current wall-clock time in the platform's configured local timezone as `jiff::Zoned`.
    fn now_local(&self) -> Option<jiff::Zoned>;

    /// Monotonic duration since system boot.
    fn duration_since_boot(&self) -> core::time::Duration;

    /// Emits audible or haptic feedback through hardware actuators.
    fn signal_feedback(&self, feedback: Feedback);

    /// Handles what the system menu asks.
    fn on_system_menu_action(&self, action: SystemMenuAction);
}
