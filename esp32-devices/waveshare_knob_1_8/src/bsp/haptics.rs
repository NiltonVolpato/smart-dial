// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! TI DRV2605 haptic feedback driver for Waveshare Knob 1.8.

use alloc::rc::Rc;

use app_shell::Feedback;
pub use common::FeedbackChannel as HapticChannel;
use drv2605::Drv2605;

/// Asynchronous Embassy task serving haptic feedback requests via DRV2605.
#[embassy_executor::task]
pub async fn haptic_task(i2c: super::SharedI2c, channel: Rc<HapticChannel>) {
    let mut drv = Drv2605::new(i2c);

    if let Err(e) = drv.init().await {
        defmt::error!("Failed to initialize DRV2605 haptic driver: {:?}", defmt::Debug2Format(&e));
        // Drain loop so the feedback channel never blocks senders
        loop {
            let _ = channel.receive().await;
        }
    }
    defmt::info!("DRV2605 haptics initialized successfully");

    // Boot click (sharp click effect 1)
    let _ = drv.play_effect(1).await;

    loop {
        let feedback = channel.receive().await;
        match feedback {
            Feedback::DialStepForward => {
                // Sharp tick 1 (effect 17)
                let _ = drv.play_effect(17).await;
            }
            Feedback::DialStepBackward => {
                // Soft tick 1 (effect 24)
                let _ = drv.play_effect(24).await;
            }
            Feedback::Click => {
                // Strong click 100% (effect 1)
                let _ = drv.play_effect(1).await;
            }
            Feedback::Haptic => {
                // Long buzz / transition ramp (effect 14)
                let _ = drv.play_effect(14).await;
            }
            Feedback::Tone { .. } => {
                // DRV2605 uses built-in ROM waveforms, map generic tone to click
                let _ = drv.play_effect(1).await;
            }
        }
    }
}
