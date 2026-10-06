//! On-device isolation test for the Slint side of rotation handling.
//!
//! Live logging proved three things hold on every observed rotation:
//! the PCNT/accumulator math is exact (raw deltas always match detents),
//! the EVENTS channel never reports a drop, and Slint always reports
//! `Accepted` for the KeyPressed we send. Yet the dial still sometimes
//! visibly does nothing. Scraping more logs from the live system can't
//! tell us anything further, since every stage we can observe already
//! reports success on every single rotation.
//!
//! This test isolates the one remaining untested link: does repeatedly
//! dispatching the exact same `WindowEvent` sequence `dispatch_input_event`
//! sends (KeyPressed then KeyReleased) against a real `LauncherApp`, with no
//! physical hardware or timing variance involved at all, ever fail to move
//! `selected` by exactly one? If this passes reliably across many
//! iterations, Slint's key handling and our dispatch wiring are exonerated
//! and the remaining search space is physical/mechanical, not software.

#![no_std]
#![no_main]

extern crate alloc;

esp_bootloader_esp_idf::esp_app_desc!();

#[cfg(test)]
#[embedded_test::tests(executor = esp_rtos::embassy::Executor::new())]
mod tests {
    use alloc::boxed::Box;
    use alloc::rc::Rc;
    use alloc::vec::Vec;

    use app_launcher::LauncherApp;
    use defmt::assert_eq;
    use lilygo_t_encoder_pro::bsp::EspPlatform;
    use lilygo_t_encoder_pro::bsp::display::{BUFFER_HEIGHT, NativeRgb565, RENDER_STRIDE};
    use slint::ComponentHandle;
    use slint::platform::software_renderer::MinimalSoftwareWindow;
    use slint::platform::{Key, WindowEvent};
    use theme::AppInfo;

    #[init]
    fn init() {
        let peripherals = esp_hal::init(esp_hal::Config::default());
        esp_alloc::heap_allocator!(size: 64 * 1024);

        let timg0 = esp_hal::timer::timg::TimerGroup::new(peripherals.TIMG0);
        let sw_interrupt =
            esp_hal::interrupt::software::SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
        esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

        rtt_target::rtt_init_defmt!();
    }

    fn dummy_cards(n: usize) -> slint::ModelRc<AppInfo> {
        let cards: Vec<AppInfo> = (0..n)
            .map(|_| AppInfo {
                name: "Test".into(),
                accent: slint::Color::from_argb_u8(255, 255, 255, 255),
                icon: slint::Image::default(),
            })
            .collect();
        Rc::new(slint::VecModel::from(cards)).into()
    }

    /// Sends exactly the same event pair `dispatch_input_event` sends for one detent.
    fn press_release(window: &Rc<MinimalSoftwareWindow>, key: Key) {
        let pressed = window
            .dispatch_event_with_result(WindowEvent::KeyPressed { text: key.into() })
            .expect("dispatch_event_with_result errored on press");
        let accepted = matches!(pressed, slint::platform::WindowEventDispatchResult::Accepted);
        assert!(accepted, "Slint did not accept the key press");

        let _ = window.dispatch_event_with_result(WindowEvent::KeyReleased { text: key.into() });
    }

    /// Mirrors steps 1 and 3 of `run_event_loop`: advance animations/timers, then render
    /// if Slint considers the window dirty. `RepaintBufferType::SwappedBuffers` requires
    /// calls to strictly alternate between two distinct buffers (its dirty-region
    /// bookkeeping assumes each buffer holds the content from two calls ago) — reusing
    /// one buffer violates that and made an earlier version of this test hang for real,
    /// so `bufs` and `toggle` here must alternate, matching production's actual scheme.
    fn tick_render(
        window: &Rc<MinimalSoftwareWindow>,
        bufs: &mut [&mut [NativeRgb565]; 2],
        toggle: &mut bool,
    ) {
        slint::platform::update_timers_and_animations();
        let buf = &mut *bufs[*toggle as usize];
        *toggle = !*toggle;
        window.draw_if_needed(|renderer| {
            let _ = renderer.render(buf, RENDER_STRIDE);
        });
    }

    #[test]
    async fn rotate_dispatch_never_skips_a_detent() {
        let (platform, window_holder) = EspPlatform::new();
        slint::platform::set_platform(Box::new(platform)).expect("platform already set");

        const CARDS: usize = 6;
        let launcher = LauncherApp::new().expect("failed to create LauncherApp");
        launcher.set_cards(dummy_cards(CARDS));
        launcher.set_selected(0);
        launcher.show().expect("failed to show LauncherApp");

        let window = window_holder.borrow().clone().expect("EspPlatform did not register a window");

        static FRAME_BUFFER_A: static_cell::ConstStaticCell<
            [NativeRgb565; RENDER_STRIDE * BUFFER_HEIGHT],
        > = static_cell::ConstStaticCell::new(
            [NativeRgb565::new(0); RENDER_STRIDE * BUFFER_HEIGHT],
        );
        static FRAME_BUFFER_B: static_cell::ConstStaticCell<
            [NativeRgb565; RENDER_STRIDE * BUFFER_HEIGHT],
        > = static_cell::ConstStaticCell::new(
            [NativeRgb565::new(0); RENDER_STRIDE * BUFFER_HEIGHT],
        );
        let mut bufs = [FRAME_BUFFER_A.take().as_mut_slice(), FRAME_BUFFER_B.take().as_mut_slice()];
        let mut toggle = false;
        tick_render(&window, &mut bufs, &mut toggle); // initial paint, like the first run_event_loop iteration

        // Sweep 0 -> CARDS-1 with UpArrow, then CARDS-1 -> 0 with DownArrow, rendering
        // after every dispatch (matching run_event_loop's update-then-render structure)
        // and asserting after every single dispatch, not just at the end, so a skip
        // points straight at the exact iteration it happened on. Repeat many times to
        // catch something intermittent rather than deterministic.
        for round in 0..200u32 {
            for expected in 1..CARDS {
                press_release(&window, Key::UpArrow);
                let selected = launcher.get_selected();
                assert_eq!(
                    selected, expected as i32,
                    "round {}: expected selected={} after UpArrow #{}, got {}",
                    round, expected, expected, selected
                );
                tick_render(&window, &mut bufs, &mut toggle);
            }
            for expected in (0..CARDS - 1).rev() {
                press_release(&window, Key::DownArrow);
                let selected = launcher.get_selected();
                assert_eq!(
                    selected, expected as i32,
                    "round {}: expected selected={} after DownArrow, got {}",
                    round, expected, selected
                );
                tick_render(&window, &mut bufs, &mut toggle);
            }
        }

        defmt::info!("rotate_dispatch_never_skips_a_detent: 200 rounds, no skips");
    }
}
