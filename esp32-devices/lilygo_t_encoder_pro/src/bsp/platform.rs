// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Slint Platform implementation and event loop for LilyGO T-Encoder Pro.

extern crate alloc;
use alloc::rc::Rc;

pub use common::{EspPlatform, WindowHolder};
use common::{FeedbackSink, run_event_loop as run_common_event_loop};
use slint::PhysicalSize;
use slint::platform::software_renderer::MinimalSoftwareWindow;

use super::buzzer;
use super::display::{
    BUFFER_HEIGHT, DISPLAY_COMMAND_CHANNEL, DirtyRect, DisplayCommand, FLUSH_RETURN_CHANNEL,
    FlushJob, Framebuffer, NativeRgb565, RENDER_HEIGHT, RENDER_STRIDE, RENDER_WIDTH,
};
use super::event::ScreenEvent;

pub struct LilygoFeedback(pub Rc<buzzer::BuzzerChannel>);
impl FeedbackSink for LilygoFeedback {
    fn on_dial_step_forward(&self) {
        let _ = self.0.try_send(buzzer::Feedback::DialStepForward);
    }
    fn on_dial_step_backward(&self) {
        let _ = self.0.try_send(buzzer::Feedback::DialStepBackward);
    }
    fn on_click(&self) {
        let _ = self.0.try_send(buzzer::Feedback::Click);
    }
    fn on_long_press(&self) {
        let _ = self.0.try_send(buzzer::Feedback::Haptic);
    }
}

pub fn create_platform() -> (EspPlatform, WindowHolder) {
    EspPlatform::with_config(
        Some(PhysicalSize::new(RENDER_WIDTH as u32, RENDER_HEIGHT as u32)),
        Some(0.5),
    )
}

#[inline(always)]
fn cycle_count() -> u32 {
    #[cfg(target_arch = "xtensa")]
    {
        xtensa_lx::timer::get_cycle_count()
    }
    #[cfg(not(target_arch = "xtensa"))]
    {
        0
    }
}

/// Runs the main Slint event loop: awaits interrupt-driven inputs, advances
/// animations, and renders updates.
pub async fn run_event_loop(window_holder: WindowHolder, feedback: &LilygoFeedback) -> ! {
    // Two framebuffers in fast internal SRAM (2 * ~78.4 KiB = ~156.8 KiB)
    #[repr(align(16))]
    struct FrameBuffer([NativeRgb565; RENDER_STRIDE * BUFFER_HEIGHT]);

    static FRAME_BUFFER_A: static_cell::ConstStaticCell<FrameBuffer> =
        static_cell::ConstStaticCell::new(FrameBuffer(
            [NativeRgb565::new(0); RENDER_STRIDE * BUFFER_HEIGHT],
        ));
    static FRAME_BUFFER_B: static_cell::ConstStaticCell<FrameBuffer> =
        static_cell::ConstStaticCell::new(FrameBuffer(
            [NativeRgb565::new(0); RENDER_STRIDE * BUFFER_HEIGHT],
        ));

    let fb_a = Framebuffer(&mut FRAME_BUFFER_A.take().0);
    let fb_b = Framebuffer(&mut FRAME_BUFFER_B.take().0);
    FLUSH_RETURN_CHANNEL
        .try_send(fb_a)
        .expect("FLUSH_RETURN_CHANNEL full while seeding framebuffer A, buffer lost");
    FLUSH_RETURN_CHANNEL
        .try_send(fb_b)
        .expect("FLUSH_RETURN_CHANNEL full while seeding framebuffer B, buffer lost");

    let base_brightness: u8 = 255;

    run_common_event_loop(
        window_holder,
        feedback,
        base_brightness,
        async |screen_event| match screen_event {
            ScreenEvent::DimRelative(ratio) => {
                let level = ((base_brightness as f32 * ratio + 0.5) as u32).clamp(1, 255) as u8;
                defmt::info!("Screen dimming to relative brightness {}", level);
                DISPLAY_COMMAND_CHANNEL.send(DisplayCommand::SetBrightness(level)).await;
            }
            ScreenEvent::DimAbsolute(level) => {
                defmt::info!("Screen dimming to absolute brightness {}", level);
                DISPLAY_COMMAND_CHANNEL.send(DisplayCommand::SetBrightness(level)).await;
            }
            ScreenEvent::TurnOff => {
                defmt::info!("Turning off display panel");
                DISPLAY_COMMAND_CHANNEL.send(DisplayCommand::DisplayOff).await;
            }
            ScreenEvent::TurnOn => {
                defmt::info!("Turning on display panel");
                DISPLAY_COMMAND_CHANNEL.send(DisplayCommand::DisplayOn).await;
                DISPLAY_COMMAND_CHANNEL
                    .send(DisplayCommand::SetBrightness(base_brightness))
                    .await;
            }
        },
        async |_action| {},
        async |window: Rc<MinimalSoftwareWindow>| {
            window
                .draw_async_if_needed(async |renderer| {
                    let fb = FLUSH_RETURN_CHANNEL.receive().await;

                    let mut rects: heapless::Vec<DirtyRect, 3> = heapless::Vec::new();
                    let r_start = cycle_count();
                    let region = renderer.render(&mut fb.0[..], RENDER_STRIDE);
                    let render_cycles = cycle_count().wrapping_sub(r_start);

                    for (origin, size) in region.iter_box() {
                        let x = origin.x.max(0) as u16;
                        let y = origin.y.max(0) as u16;
                        let width = size.width as u16;
                        let height = size.height as u16;

                        let _ = rects.push(DirtyRect { x, y, width, height });
                    }

                    DISPLAY_COMMAND_CHANNEL
                        .send(DisplayCommand::Flush(FlushJob { fb, rects, render_cycles }))
                        .await;
                })
                .await
        },
    )
    .await
}
