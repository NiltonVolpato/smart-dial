// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Board Support Package (BSP) for LilyGO T-Encoder Pro (ESP32-S3).

pub mod board;
pub mod buzzer;
pub mod display;
pub mod event;
pub mod input;
pub mod platform;
pub mod rotary;
pub use common::simd;
pub mod touch;

pub use board::{
    Board, BuzzerPeripherals, Core0Peripherals, Core1Peripherals, DisplayPeripherals,
    EncoderPeripherals, SystemPeripherals, TouchPeripherals,
};
pub use buzzer::{BuzzerChannel, Feedback, buzzer_task};
pub use display::{
    BigEndianRgb565, Co5300, DISPLAY_HEIGHT, DISPLAY_WIDTH, RENDER_HEIGHT, RENDER_WIDTH,
    TX_BUF_BYTES, display_task,
};
use esp_hal::gpio::{Input, InputConfig, Pull};
use esp_hal::i2c::master::{BusTimeout, Config as I2cConfig, I2c};
use esp_hal::time::Rate;
pub use event::{EVENTS, Event, ScreenEvent, send_event};
pub use input::{InputEvent, send_input_event};
pub use platform::{EspPlatform, LilygoFeedback, WindowHolder, run_event_loop};
pub use rotary::{EncoderHw, button_task, init_rotary, rotary_decode_once, rotary_task};
pub use touch::{Chsc5816, touch_task};

pub struct Bsp {
    pub window: WindowHolder,
    pub touch: Option<Chsc5816>,
    pub button: Input<'static>,
    pub buzzer: BuzzerPeripherals,
    pub profiler_timer: esp_hal::peripherals::TIMG1<'static>,
}

impl Bsp {
    /// Initializes touch, rotary, profiler, and registers the Slint platform on Core 0.
    pub fn init(core0: Core0Peripherals) -> Self {
        // 1. Initialize async I2C0 for CHSC5816 touch controller
        let touch = match I2c::new(
            core0.touch.i2c,
            I2cConfig::default()
                .with_frequency(Rate::from_khz(400))
                .with_timeout(BusTimeout::Maximum),
        ) {
            Ok(i2c) => {
                let i2c = i2c.with_sda(core0.touch.sda).with_scl(core0.touch.scl).into_async();
                let touch_dev = Chsc5816::new(i2c, core0.touch.int, core0.touch.rst);
                Some(touch_dev)
            }
            Err(e) => {
                defmt::error!("I2C0 initialization failed: {:?}", defmt::Debug2Format(&e));
                None
            }
        };

        // 2. Initialize PCNT quadrature rotary encoder (fully interrupt-driven) and button
        let encoder_hw =
            EncoderHw::new(core0.encoder.pcnt, core0.encoder.pin_a, core0.encoder.pin_b);
        init_rotary(core0.encoder.io_mux, encoder_hw);
        let button = Input::new(core0.encoder.button, InputConfig::default().with_pull(Pull::Up));

        // 3. Set Slint platform
        let (platform, window) = platform::create_platform();
        slint::platform::set_platform(alloc::boxed::Box::new(platform))
            .expect("Slint platform already set");

        // 4. Enable PIE SIMD coprocessor
        simd::enable_pie();

        Self {
            window,
            touch,
            button,
            buzzer: core0.buzzer,
            profiler_timer: core0.profiler_timer,
        }
    }
}
