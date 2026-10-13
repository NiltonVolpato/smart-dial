// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Slint Platform implementation and event loop for Waveshare ESP32-S3-Knob-Touch-LCD-1.8.

extern crate alloc;
use alloc::rc::Rc;

use common::event::ScreenEvent;
use common::{
    BigEndianRgb565, EspPlatform, FeedbackSink, WindowHolder,
    run_event_loop as run_common_event_loop,
};
use slint::PhysicalSize;
use slint::platform::software_renderer::MinimalSoftwareWindow;

use super::display::{
    DISPLAY_COMMAND_CHANNEL, DirtyRect, DisplayCommand, FLUSH_RETURN_CHANNEL, FlushJob,
    Framebuffer, RENDER_HEIGHT, RENDER_STRIDE, RENDER_WIDTH,
};
use super::haptics;

pub struct WaveshareFeedback(pub Rc<haptics::HapticChannel>);
impl FeedbackSink for WaveshareFeedback {
    fn on_dial_step_forward(&self) {
        let _ = self.0.try_send(app_shell::Feedback::DialStepForward);
    }
    fn on_dial_step_backward(&self) {
        let _ = self.0.try_send(app_shell::Feedback::DialStepBackward);
    }
    fn on_click(&self) {
        let _ = self.0.try_send(app_shell::Feedback::Click);
    }
    fn on_long_press(&self) {
        let _ = self.0.try_send(app_shell::Feedback::Haptic);
    }
}

pub(crate) fn create_platform() -> (EspPlatform, WindowHolder) {
    EspPlatform::with_config(
        Some(PhysicalSize::new(RENDER_WIDTH as u32, RENDER_HEIGHT as u32)),
        Some(1.0), // Native 1:1 scaling for crisp, unaliased rendering
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
pub async fn run_event_loop(
    window_holder: WindowHolder,
    feedback: &WaveshareFeedback,
    wifi_client: crate::bsp::coprocessor::WifiClient,
    shell: Rc<core::cell::RefCell<app_shell::AppShell>>,
    sources: common::SystemSources,
) -> ! {
    // Single framebuffer in fast internal SRAM (259,200 bytes = 253.125 KiB)
    #[repr(align(16))]
    struct FrameBuffer([BigEndianRgb565; RENDER_STRIDE * RENDER_HEIGHT as usize]);

    static FRAME_BUFFER: static_cell::ConstStaticCell<FrameBuffer> =
        static_cell::ConstStaticCell::new(FrameBuffer(
            [BigEndianRgb565(0); RENDER_STRIDE * RENDER_HEIGHT as usize],
        ));

    let fb = Framebuffer(&mut FRAME_BUFFER.take().0);
    FLUSH_RETURN_CHANNEL
        .try_send(fb)
        .expect("FLUSH_RETURN_CHANNEL full while seeding framebuffer");

    let settings = common::storage::init().await;
    let brightness_percent = settings.brightness_percent as i32;
    let base_brightness = theme::brightness_percent_to_duty(brightness_percent);

    run_common_event_loop(
        window_holder,
        feedback,
        shell,
        brightness_percent,
        sources,
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
        async |menu_action| match menu_action {
            theme::SystemMenuAction::RequestProvisioning => {
                defmt::info!("[MENU] Starting co-processor BLE provisioning (60s)");
                let mut req = coprocessor::proto::StartProvisioningRequestT::default();
                req.timeout_seconds = 60;
                // The menu follows the session's state, from System.Watch.
                let _ = wifi_client.start_provisioning(&req).await;
            }
            theme::SystemMenuAction::StopProvisioning => {
                defmt::info!("[MENU] Stopping co-processor BLE provisioning");
                let _ = wifi_client.stop_provisioning(&coprocessor::proto::EmptyT::default()).await;
            }
            _ => {}
        },
        async |window: Rc<MinimalSoftwareWindow>| {
            window
                .draw_async_if_needed(async |renderer| {
                    let fb = FLUSH_RETURN_CHANNEL.receive().await;

                    let mut rects: heapless::Vec<DirtyRect, 4> = heapless::Vec::new();
                    let r_start = cycle_count();
                    let region = renderer.render(&mut fb.0[..], RENDER_STRIDE);
                    let render_cycles = cycle_count().wrapping_sub(r_start);

                    let mut y_min = RENDER_HEIGHT;
                    let mut y_max = 0;
                    for (origin, size) in region.iter_box() {
                        let y0 = origin.y.max(0) as u16;
                        let y1 = (origin.y + size.height as i32).max(0) as u16;
                        if y0 < RENDER_HEIGHT && y1 > 0 {
                            y_min = y_min.min(y0);
                            y_max = y_max.max(y1.min(RENDER_HEIGHT));
                        }
                    }

                    if y_min < y_max {
                        // SH8601 QSPI requires even start coordinate and even height
                        let y = (y_min / 2) * 2;
                        let bottom = y_max.div_ceil(2) * 2;
                        let height = bottom.min(RENDER_HEIGHT).saturating_sub(y);
                        if height > 0 {
                            let _ = rects.push(DirtyRect { x: 0, y, width: RENDER_WIDTH, height });
                        }
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
