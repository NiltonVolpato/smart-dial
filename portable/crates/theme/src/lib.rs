#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;

use slint::ComponentHandle;

slint::include_modules!();

/// What the system menu asks of the platform.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum SystemMenuAction {
    RequestProvisioning,
    StopProvisioning,
    /// Already clamped (see `clamp_brightness`), and shown by the menu.
    BrightnessChanged(i32),
    MenuClosed,
}

/// A handle to one app window's `System` global, to set the state it shows.
/// Does nothing once the window is gone.
pub struct SystemGlobal(Box<dyn Fn(&SystemState)>);

impl SystemGlobal {
    pub fn set_state(&self, state: &SystemState) {
        (self.0)(state);
    }
}

/// Connects `app`'s `System` global: its actions go to `on_action`, and the
/// handle returned sets the state it shows.
pub fn connect_system<T>(app: &T, on_action: impl Fn(SystemMenuAction) + 'static) -> SystemGlobal
where
    T: ComponentHandle + 'static,
    for<'a> System<'a>: slint::Global<'a, T>,
{
    let on_action = Rc::new(on_action);
    let system = System::get(app);
    let act = on_action.clone();
    system.on_request_provisioning(move || act(SystemMenuAction::RequestProvisioning));
    let act = on_action.clone();
    system.on_stop_provisioning(move || act(SystemMenuAction::StopProvisioning));
    let act = on_action.clone();
    system.on_brightness_changed(move |percent| {
        act(SystemMenuAction::BrightnessChanged(clamp_brightness(percent)));
    });
    system.on_menu_closed(move || on_action(SystemMenuAction::MenuClosed));

    let app = app.as_weak();
    SystemGlobal(Box::new(move |state| {
        let Some(app) = app.upgrade() else { return };
        let system = System::get(&app);
        // Only on change: whatever reads any field re-evaluates.
        if system.get_state() != *state {
            system.set_state(state.clone());
        }
    }))
}

/// Connects `app`'s `Navigation` global: exiting, and haptics at the edge
/// gestures' threshold.
pub fn connect_navigation<T: ComponentHandle + 'static>(
    app: &T,
    on_exit: impl Fn() + 'static,
    on_threshold: impl Fn() + 'static,
) where
    for<'a> Navigation<'a>: slint::Global<'a, T>,
{
    let nav = Navigation::get(app);
    nav.on_exit(on_exit);
    nav.on_threshold_reached(on_threshold);
}

/// Minimum allowable display brightness percentage (prevents complete blackouts).
pub const MIN_BRIGHTNESS_PERCENT: i32 = 15;

/// Maximum allowable display brightness percentage.
pub const MAX_BRIGHTNESS_PERCENT: i32 = 100;

/// Clamps brightness percentage to safe bounds [15, 100].
#[inline]
pub const fn clamp_brightness(percent: i32) -> i32 {
    if percent < MIN_BRIGHTNESS_PERCENT {
        MIN_BRIGHTNESS_PERCENT
    } else if percent > MAX_BRIGHTNESS_PERCENT {
        MAX_BRIGHTNESS_PERCENT
    } else {
        percent
    }
}

// TODO: the PWM conversions below are the Waveshare LCD's: they belong in its
// display driver, behind a device abstraction that takes a percent. (Lilygo's
// AMOLED takes the percent as is.)

/// Converts brightness percent (15..=100) to 8-bit PWM duty cycle (0..=255).
#[inline]
pub const fn brightness_percent_to_duty(percent: i32) -> u8 {
    let clamped = clamp_brightness(percent);
    ((clamped as u32 * 255) / 100) as u8
}

/// Converts 8-bit PWM duty cycle (0..=255) to brightness percent (15..=100).
#[inline]
pub const fn duty_to_brightness_percent(duty: u8) -> i32 {
    let pct = ((duty as u32 * 100 + 127) / 255) as i32;
    clamp_brightness(pct)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_clamp_brightness() {
        assert_eq!(clamp_brightness(0), 15);
        assert_eq!(clamp_brightness(10), 15);
        assert_eq!(clamp_brightness(15), 15);
        assert_eq!(clamp_brightness(50), 50);
        assert_eq!(clamp_brightness(100), 100);
        assert_eq!(clamp_brightness(120), 100);
    }

    #[test]
    fn test_brightness_duty_conversion() {
        // Minimum brightness 15% should be > 0 duty (never dark)
        let min_duty = brightness_percent_to_duty(15);
        assert_eq!(min_duty, 38);
        assert!(min_duty > 0);

        // Maximum brightness 100% should be full 255 duty
        let max_duty = brightness_percent_to_duty(100);
        assert_eq!(max_duty, 255);

        // Clamping ensures out-of-range values produce safe duty
        assert_eq!(brightness_percent_to_duty(0), 38);
        assert_eq!(brightness_percent_to_duty(200), 255);

        // Duty to percent converts correctly
        assert_eq!(duty_to_brightness_percent(0), 15);
        assert_eq!(duty_to_brightness_percent(38), 15);
        assert_eq!(duty_to_brightness_percent(255), 100);
    }

    #[test]
    fn test_drawer_position_math() {
        let drawer_height: f32 = 315.0;
        let drawer_open_y: f32 = 0.0;
        let threshold: f32 = 180.0;

        let calc_y = |drag_dist: f32| -> f32 {
            let p = (drag_dist / threshold).min(1.0);
            -drawer_height + p * (drawer_height + drawer_open_y)
        };

        // When drag starts, drawer is hidden above screen
        assert_eq!(calc_y(0.0), -315.0);
        // Halfway through drag
        assert_eq!(calc_y(90.0), -157.5);
        // At threshold, drawer is at full open position (0.0)
        assert_eq!(calc_y(180.0), 0.0);
        // Past threshold, drawer is clamped to open position
        assert_eq!(calc_y(220.0), 0.0);
    }
}
