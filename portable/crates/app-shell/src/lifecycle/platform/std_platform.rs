// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Standard library platform implementation for host testing and simulators.

use std::time::Instant;

use theme::AppInfo;

use super::Platform;

/// Default standard library platform implementation for host and simulators.
#[derive(Clone, Debug)]
pub struct StdPlatform {
    start_time: Instant,
}

impl StdPlatform {
    /// Creates a new `StdPlatform` recording the current instant as start time.
    pub fn new() -> Self {
        Self { start_time: Instant::now() }
    }

    /// Creates an Embassy executor on the current thread and returns its `Spawner`.
    pub fn host_spawner() -> embassy_executor::Spawner {
        let executor = Box::leak(Box::new(embassy_executor::Executor::new()));
        let mut spawner_opt = None;
        executor.run_until(
            |spawner| {
                spawner_opt = Some(spawner);
            },
            || true,
        );
        spawner_opt.expect("Spawner should be initialized")
    }
}

impl Default for StdPlatform {
    fn default() -> Self {
        Self::new()
    }
}

impl Platform for StdPlatform {
    fn on_app_launched(&self, _: &AppInfo) {}

    fn wake_event_loop(&self) {}

    fn now_utc(&self) -> Option<jiff::Timestamp> {
        Some(jiff::Timestamp::now())
    }

    fn now_local(&self) -> Option<jiff::Zoned> {
        Some(jiff::Zoned::now())
    }

    fn duration_since_boot(&self) -> core::time::Duration {
        self.start_time.elapsed()
    }

    fn signal_feedback(&self, _: crate::feedback::Feedback) {}
}
