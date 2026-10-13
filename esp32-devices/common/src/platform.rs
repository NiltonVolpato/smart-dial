// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Slint platform adapter and generic event loop for ESP32 devices.

extern crate alloc;

use alloc::rc::Rc;
use core::cell::RefCell;
use core::future::Future;

use embassy_futures::select::{Either, Either3, select, select3};
use embassy_sync::blocking_mutex::raw::NoopRawMutex;
use embassy_sync::watch::Receiver;
use embassy_time::{Duration, Instant, Timer};
use i_slint_renderer_software::MinimalSoftwareWindow;
use slint::platform::{Key, PointerEventButton, WindowEvent};

use crate::channels::{receive_event, report_user_activity, try_receive_event};
use crate::event::{Event, InputEvent, ScreenEvent, TouchEvent};
use crate::system::{Battery, SystemSources, Wifi};

/// Handle to the active Slint window.
pub type WindowHolder = Rc<RefCell<Option<Rc<MinimalSoftwareWindow>>>>;

/// Trait for physical feedback (buzzers, vibration motors, haptic actuators).
pub trait FeedbackSink {
    fn on_dial_step_forward(&self);
    fn on_dial_step_backward(&self);
    fn on_click(&self);
    fn on_long_press(&self);
}

/// No-op feedback sink for testing or headless runs.
pub struct NoFeedback;
impl FeedbackSink for NoFeedback {
    fn on_dial_step_forward(&self) {}
    fn on_dial_step_backward(&self) {}
    fn on_click(&self) {}
    fn on_long_press(&self) {}
}

/// Slint platform adapter for ESP32 devices.
pub struct EspPlatform {
    window: WindowHolder,
    start_time: Instant,
    size: Option<slint::PhysicalSize>,
    scale_factor: Option<f32>,
}

impl EspPlatform {
    pub fn new() -> (Self, WindowHolder) {
        Self::with_config(None, None)
    }

    pub fn with_config(
        size: Option<slint::PhysicalSize>,
        scale_factor: Option<f32>,
    ) -> (Self, WindowHolder) {
        let window = WindowHolder::default();
        let platform =
            Self { window: window.clone(), start_time: Instant::now(), size, scale_factor };
        (platform, window)
    }
}

impl slint::platform::Platform for EspPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn slint::platform::WindowAdapter>, slint::PlatformError> {
        let window = MinimalSoftwareWindow::new(
            slint::platform::software_renderer::RepaintBufferType::SwappedBuffers,
        );
        if let Some(scale) = self.scale_factor {
            window.dispatch_event(WindowEvent::ScaleFactorChanged { scale_factor: scale });
        }
        if let Some(size) = self.size {
            window.set_size(size);
        }
        self.window.borrow_mut().replace(window.clone());
        Ok(window)
    }

    fn duration_since_start(&self) -> core::time::Duration {
        core::time::Duration::from_micros((Instant::now() - self.start_time).as_micros())
    }
}

fn dispatch(window: &Rc<MinimalSoftwareWindow>, event: WindowEvent) {
    let _ = window.dispatch_event_with_result(event);
}

/// Dispatches an input event to Slint with appropriate tactile feedback.
pub fn dispatch_input_event(
    window: &Rc<MinimalSoftwareWindow>,
    event: InputEvent,
    feedback: &impl FeedbackSink,
) {
    match event {
        InputEvent::Rotate(delta) => {
            if delta > 0 {
                feedback.on_dial_step_forward();
                for _ in 0..delta {
                    dispatch(window, WindowEvent::KeyPressed { text: Key::UpArrow.into() });
                    dispatch(window, WindowEvent::KeyReleased { text: Key::UpArrow.into() });
                }
            } else if delta < 0 {
                feedback.on_dial_step_backward();
                for _ in 0..(-delta) {
                    dispatch(window, WindowEvent::KeyPressed { text: Key::DownArrow.into() });
                    dispatch(window, WindowEvent::KeyReleased { text: Key::DownArrow.into() });
                }
            }
        }
        InputEvent::Click => {
            feedback.on_click();
            dispatch(window, WindowEvent::KeyPressed { text: Key::Return.into() });
            dispatch(window, WindowEvent::KeyReleased { text: Key::Return.into() });
        }
        InputEvent::LongPress => {
            feedback.on_long_press();
            dispatch(window, WindowEvent::KeyPressed { text: Key::Escape.into() });
            dispatch(window, WindowEvent::KeyReleased { text: Key::Escape.into() });
        }
        InputEvent::Touch(point) => {
            let position = slint::LogicalPosition::new(point.x as f32, point.y as f32);
            match point.event {
                TouchEvent::Down => {
                    dispatch(
                        window,
                        WindowEvent::PointerPressed { position, button: PointerEventButton::Left },
                    );
                }
                TouchEvent::Move => {
                    dispatch(window, WindowEvent::PointerMoved { position });
                }
                TouchEvent::Up => {
                    dispatch(
                        window,
                        WindowEvent::PointerReleased { position, button: PointerEventButton::Left },
                    );
                    dispatch(window, WindowEvent::PointerExited);
                }
            }
        }
    }
}

/// A change from one of the system menu's sources.
enum SystemChange {
    Wifi(Wifi),
    Provisioning(bool),
    Battery(Battery),
}

async fn next_system_change(
    sources: &SystemSources,
    battery: &mut Receiver<'_, NoopRawMutex, Battery, 1>,
) -> SystemChange {
    match select3(sources.wifi.wait(), sources.provisioning.wait(), battery.changed()).await {
        Either3::First(wifi) => SystemChange::Wifi(wifi),
        Either3::Second(active) => SystemChange::Provisioning(active),
        Either3::Third(battery) => SystemChange::Battery(battery),
    }
}

async fn next_event(timeout: Option<Duration>) -> Option<Event> {
    let Some(timeout) = timeout else {
        return Some(receive_event().await);
    };
    match select(receive_event(), Timer::after(timeout)).await {
        Either::First(event) => Some(event),
        Either::Second(()) => None,
    }
}

/// Generic event loop for ESP32 Slint devices.
///
/// Handles event draining, Slint timers and animations, input dispatching,
/// sleep/wake state management, and delegates frame rendering to `render_frame`.
#[expect(
    clippy::too_many_arguments,
    reason = "temporary fix: group the device hooks (screen, menu, render) into one trait"
)]
pub async fn run_event_loop<F, S, SF, M, MF, RF, RFF>(
    window_holder: WindowHolder,
    feedback: &F,
    shell: Rc<RefCell<app_shell::AppShell>>,
    brightness_percent: i32,
    sources: SystemSources,
    mut on_screen_event: S,
    mut on_menu_action: M,
    mut render_frame: RF,
) -> !
where
    F: FeedbackSink,
    S: FnMut(ScreenEvent) -> SF,
    SF: Future<Output = ()>,
    M: FnMut(theme::SystemMenuAction) -> MF,
    MF: Future<Output = ()>,
    RF: FnMut(Rc<MinimalSoftwareWindow>) -> RFF,
    RFF: Future<Output = bool>,
{
    let mut pending_event: Option<Event> = None;
    let mut is_dimmed: bool = false;
    let mut is_sleeping: bool = false;
    let mut base_brightness = theme::brightness_percent_to_duty(brightness_percent);

    // What the system menu shows.
    let mut system =
        theme::SystemState { battery_percent: -1, brightness_percent, ..Default::default() };
    app_shell::AppShell::set_system_state(&shell, &system);
    let mut battery = sources.battery.receiver().expect("The event loop is the battery's receiver");

    defmt::info!("Entering unified Slint MCU event loop");

    loop {
        // 1. Advance Slint animations and timers
        slint::platform::update_timers_and_animations();

        let Some(window) = window_holder.borrow().clone() else {
            Timer::after(Duration::from_millis(10)).await;
            continue;
        };

        // 2b. Process ALL pending events before drawing
        let mut event = pending_event.take().or_else(try_receive_event);
        while let Some(current_event) = event {
            match current_event {
                Event::Input(input) => {
                    report_user_activity();

                    if is_sleeping {
                        defmt::info!("Waking display from sleep");
                        on_screen_event(ScreenEvent::TurnOn).await;
                        on_screen_event(ScreenEvent::DimAbsolute(base_brightness)).await;
                        is_sleeping = false;
                        is_dimmed = false;
                        window.request_redraw();
                    } else {
                        if is_dimmed {
                            defmt::info!("Restoring screen brightness on user interaction");
                            on_screen_event(ScreenEvent::DimAbsolute(base_brightness)).await;
                            is_dimmed = false;
                        }
                        dispatch_input_event(&window, input, feedback);
                    }
                }
                Event::Screen(screen_event) => {
                    match screen_event {
                        ScreenEvent::DimRelative(_) | ScreenEvent::DimAbsolute(_) => {
                            is_dimmed = true;
                        }
                        ScreenEvent::TurnOff => {
                            is_sleeping = true;
                        }
                        ScreenEvent::TurnOn => {
                            is_sleeping = false;
                            is_dimmed = false;
                            window.request_redraw();
                        }
                    }
                    on_screen_event(screen_event).await;
                }
                Event::WakeUp => {
                    defmt::debug!("WakeUp event received");
                }
                Event::SystemMenu(theme::SystemMenuAction::BrightnessChanged(percent)) => {
                    // The menu shows it already.
                    system.brightness_percent = percent;
                    app_shell::AppShell::set_system_state(&shell, &system);
                    base_brightness = theme::brightness_percent_to_duty(percent);
                    on_screen_event(ScreenEvent::DimAbsolute(base_brightness)).await;
                }
                Event::SystemMenu(theme::SystemMenuAction::MenuClosed) => {
                    let _ = crate::storage::save_settings(crate::storage::SystemSettings {
                        brightness_percent: system.brightness_percent as u8,
                    })
                    .await;
                }
                Event::SystemMenu(action) => {
                    on_menu_action(action).await;
                }
            }
            event = try_receive_event();
        }

        // 3. Render dirty regions (skip DMA transfers if screen is sleeping)
        let mut drew_frame = false;
        if !is_sleeping {
            drew_frame = render_frame(window.clone()).await;
        }

        // 4. Determine timeout for event wait
        let animating = !is_sleeping && (window.has_active_animations() || drew_frame);
        let timeout = if animating {
            Some(Duration::from_hz(60))
        } else {
            slint::platform::duration_until_next_timer_update()
                .map(|d| Duration::from_micros(d.as_micros() as u64))
        };

        // 5. Await next event, system change, or animation/timer tick
        match select(next_event(timeout), next_system_change(&sources, &mut battery)).await {
            Either::First(event) => pending_event = event,
            Either::Second(change) => {
                match change {
                    SystemChange::Wifi(wifi) => {
                        system.wifi = wifi.state;
                        system.wifi_ssid = wifi.ssid;
                    }
                    SystemChange::Provisioning(active) => system.is_provisioning = active,
                    SystemChange::Battery(battery) => {
                        system.battery_percent = battery.percent;
                        system.is_plugged_in = battery.is_plugged;
                    }
                }
                app_shell::AppShell::set_system_state(&shell, &system);
            }
        }
    }
}

/// Physical feedback channel shared across Embassy tasks on Core 0.
pub type FeedbackChannel = embassy_sync::channel::Channel<
    embassy_sync::blocking_mutex::raw::NoopRawMutex,
    app_shell::Feedback,
    16,
>;

/// Application shell platform integration for ESP32 devices.
///
/// Logs application launch events and heap statistics via `defmt`.
pub struct EspAppShellPlatform {
    start_time: esp_hal::time::Instant,
    current_utc_fn: Option<fn() -> Option<jiff::Timestamp>>,
    current_local_fn: Option<fn() -> Option<jiff::Zoned>>,
    feedback_channel: Option<Rc<FeedbackChannel>>,
}

impl EspAppShellPlatform {
    pub fn new(
        current_utc_fn: Option<fn() -> Option<jiff::Timestamp>>,
        current_local_fn: Option<fn() -> Option<jiff::Zoned>>,
        feedback_channel: Option<Rc<FeedbackChannel>>,
    ) -> Self {
        Self {
            start_time: esp_hal::time::Instant::now(),
            current_utc_fn,
            current_local_fn,
            feedback_channel,
        }
    }
}

impl app_shell::Platform for EspAppShellPlatform {
    fn on_app_launched(&self, info: &theme::AppInfo) {
        defmt::debug!("Launched app: {}, {}", info.name.as_str(), esp_alloc::HEAP.stats());
    }

    fn wake_event_loop(&self) {
        crate::channels::wake_event_loop();
    }

    fn now_utc(&self) -> Option<jiff::Timestamp> {
        self.current_utc_fn.and_then(|f| f())
    }

    fn now_local(&self) -> Option<jiff::Zoned> {
        self.current_local_fn.and_then(|f| f())
    }

    fn duration_since_boot(&self) -> core::time::Duration {
        core::time::Duration::from_micros(
            (esp_hal::time::Instant::now() - self.start_time).as_micros(),
        )
    }

    fn signal_feedback(&self, feedback: app_shell::Feedback) {
        if let Some(ref channel) = self.feedback_channel {
            let _ = channel.try_send(feedback);
        }
    }

    fn on_system_menu_action(&self, action: theme::SystemMenuAction) {
        crate::channels::send_event(Event::SystemMenu(action));
    }
}
