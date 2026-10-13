// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Shared platform layer and tasks for ESP32 devices running Slint.

#![no_std]
#![feature(asm_experimental_arch)]

extern crate alloc;

pub mod channels;
pub mod event;
pub mod pixel;
pub mod platform;
pub mod simd;
pub mod storage;
pub mod system;
pub mod tasks;

pub use channels::*;
pub use event::*;
pub use pixel::*;
pub use platform::*;
pub use storage::*;
pub use system::*;
