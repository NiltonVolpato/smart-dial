// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Bidirectional pulsed switch rotary encoder driver for Waveshare ESP32-S3-Knob-Touch-LCD-1.8.

use common::channels::send_input_event;
use common::event::InputEvent;
use embassy_futures::select::{Either, select};
use embassy_time::Timer;
use esp_hal::gpio::Input;

/// Per-channel debounce state machine from Waveshare bidi_switch_knob reference.
#[derive(Clone, Copy, Debug)]
struct ChannelDebounce {
    prev_level: bool,
    debounce_cnt: u8,
}

impl ChannelDebounce {
    const fn new(initial: bool) -> Self {
        Self { prev_level: initial, debounce_cnt: 0 }
    }

    /// Processes level sample every 3ms. Returns true when a debounced transition completes.
    fn process(&mut self, current_level: bool) -> bool {
        let mut triggered = false;
        if !current_level {
            // Level is low (switch closed)
            if current_level != self.prev_level {
                self.debounce_cnt = 0;
            } else {
                self.debounce_cnt = self.debounce_cnt.saturating_add(1);
            }
        } else {
            // Level is high (switch released/open)
            if current_level != self.prev_level {
                self.debounce_cnt = self.debounce_cnt.saturating_add(1);
                if self.debounce_cnt >= 2 {
                    triggered = true;
                }
            } else {
                self.debounce_cnt = 0;
            }
        }
        self.prev_level = current_level;
        triggered
    }
}

/// Asynchronous Embassy task reading the bidirectional switch knob with 3ms reference debounce.
#[embassy_executor::task]
pub async fn rotary_task(mut pin_a: Input<'static>, mut pin_b: Input<'static>) {
    let mut ch_a = ChannelDebounce::new(pin_a.is_high());
    let mut ch_b = ChannelDebounce::new(pin_b.is_high());

    loop {
        // Sleep on edge interrupt when both pins are resting high
        if pin_a.is_high() && pin_b.is_high() {
            match select(pin_a.wait_for_falling_edge(), pin_b.wait_for_falling_edge()).await {
                Either::First(()) | Either::Second(()) => {}
            }
        }

        // Active 3ms polling loop during rotation and settling
        let mut idle_ticks = 0u32;
        while idle_ticks < 10 {
            Timer::after_millis(3).await;
            let a = pin_a.is_high();
            let b = pin_b.is_high();

            if a && b {
                idle_ticks += 1;
            } else {
                idle_ticks = 0;
            }

            if ch_a.process(a) {
                #[cfg(feature = "rotary-debug")]
                defmt::info!("[ROTARY] +1 (CW)");
                send_input_event(InputEvent::Rotate(1));
            }
            if ch_b.process(b) {
                #[cfg(feature = "rotary-debug")]
                defmt::info!("[ROTARY] -1 (CCW)");
                send_input_event(InputEvent::Rotate(-1));
            }
        }
    }
}
