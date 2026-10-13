// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Inactivity screensaver task managing automated screen dimming and sleeping.

use embassy_futures::select::{Either, select};
use embassy_sync::watch::DynReceiver;
use embassy_time::{Duration, Timer};

use crate::channels::{send_screen_event, subscribe_user_activity};
use crate::event::ScreenEvent;

#[derive(Clone, Copy, Debug)]
pub struct InactivityStage {
    pub timeout: Duration,
    pub action: ScreenEvent,
}

const STAGES: [InactivityStage; 3] = [
    InactivityStage {
        timeout: Duration::from_secs(60), // Dim screen to 50% in 1 minute
        action: ScreenEvent::DimRelative(0.50),
    },
    InactivityStage {
        timeout: Duration::from_secs(15), // Dim screen to 25% in 1:15 minute
        action: ScreenEvent::DimRelative(0.25),
    },
    InactivityStage {
        timeout: Duration::from_secs(15), // Turn off screen in 1:30 minute
        action: ScreenEvent::TurnOff,
    },
];

enum TimeoutState {
    Reset,
    TimedOut,
}

async fn wait_for_activity(
    activity: &mut DynReceiver<'static, ()>,
    optional_timeout: Option<&Duration>,
) -> TimeoutState {
    let Some(timeout) = optional_timeout else {
        activity.changed().await;
        return TimeoutState::Reset;
    };
    match select(activity.changed(), Timer::after(*timeout)).await {
        Either::First(_) => TimeoutState::Reset,
        Either::Second(_) => TimeoutState::TimedOut,
    }
}

/// Asynchronous task monitoring user activity and issuing screen dim/off events.
#[embassy_executor::task]
pub async fn screensaver_task(battery: alloc::rc::Rc<crate::system::BatteryWatch>) {
    let mut battery = battery.anon_receiver();
    let mut activity = subscribe_user_activity().expect("Failed to subscribe to user activity");
    let mut stages = STAGES.iter();
    loop {
        let current_stage = stages.next();
        match wait_for_activity(&mut activity, current_stage.map(|s| &s.timeout)).await {
            // User interacted; reset all inactivity timers.
            TimeoutState::Reset => {
                stages = STAGES.iter();
            }
            // Timer expired without interaction; report and wait for next timeout.
            TimeoutState::TimedOut => {
                if let Some(stage) = current_stage {
                    if stage.action == ScreenEvent::TurnOff
                        && battery.try_get().is_some_and(|b| b.is_plugged)
                    {
                        // Stop at 25% dimming when plugged in; do not turn off display panel.
                        continue;
                    }
                    send_screen_event(stage.action);
                }
            }
        }
    }
}
