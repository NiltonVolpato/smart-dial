// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! System feedback definitions (buzzer, haptics, and musical tones).

/// Audible and haptic feedback requests.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Feedback {
    /// Forward / clockwise dial rotation detent (523 Hz -> 659 Hz).
    DialStepForward,
    /// Backward / counter-clockwise dial rotation detent (659 Hz -> 523 Hz).
    DialStepBackward,
    /// Button click / press (crisp blip: 880 Hz for 15ms).
    Click,
    /// Low-frequency vibration buzz (e.g., long press, alarm).
    Haptic,
    /// Arbitrary frequency tone for musical or game feedback.
    Tone { hz: u32, ms: u32 },
}
