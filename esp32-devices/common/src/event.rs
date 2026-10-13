// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Unified event models for embedded device interactions.

#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum TouchEvent {
    Down,
    Move,
    Up,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct TouchPoint {
    pub x: u16,
    pub y: u16,
    pub event: TouchEvent,
}

/// User input events from physical controls.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum InputEvent {
    /// Rotary dial rotated by delta detents (+1 clockwise, -1 counter-clockwise).
    Rotate(i32),
    /// Dial button short click (pressed and released within threshold).
    Click,
    /// Dial button long press (held past threshold).
    LongPress,
    /// Touch event on capacitive panel.
    Touch(TouchPoint),
}

/// Screen display power and brightness events.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ScreenEvent {
    /// Dim display relative to user's configured base brightness (e.g. 0.50 for 50%, 0.25 for 25%).
    DimRelative(f32),
    /// Set absolute brightness level (0..255).
    DimAbsolute(u8),
    /// Turn off display panel (sleep mode).
    TurnOff,
    /// Turn on display panel (wake mode).
    TurnOn,
}

/// Unified system events dispatched through the central main loop.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Event {
    /// User input interaction.
    Input(InputEvent),
    /// Screen power/brightness change request.
    Screen(ScreenEvent),
    /// Wake up request from background task or property changes.
    WakeUp,
    /// What the system menu asks of the platform.
    SystemMenu(theme::SystemMenuAction),
}
