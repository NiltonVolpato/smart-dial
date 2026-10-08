// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

#[allow(clippy::all)]
mod coprocessor_generated;
#[allow(clippy::all, dead_code)]
mod coprocessor_rpc;

pub use coprocessor_generated::coprocessor_proto as proto;
pub use coprocessor_rpc::coprocessor_proto::{sonos, system, wifi};

#[cfg(test)]
mod tests;
