// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Hardware Real-Time Clock (RTC) and timezone handling for Waveshare Knob 1.8.

use core::cell::RefCell;

use critical_section::Mutex;
use defmt::info;
use esp_hal::peripherals::LPWR;
use esp_hal::rtc_cntl::Rtc;

const TIMEZONE: jiff::tz::TimeZone = jiff::tz::get!("America/Los_Angeles");

static RTC: Mutex<RefCell<Option<Rtc<'static>>>> = Mutex::new(RefCell::new(None));

/// Initializes the hardware RTC peripheral and registers the system time provider.
pub fn init(lpwr: LPWR<'static>) {
    let rtc = Rtc::new(lpwr);

    // Display initial hardware RTC time before network synchronization
    let initial_us = rtc.current_time_us();
    if let Ok(now) = jiff::Timestamp::from_microsecond(initial_us as i64) {
        info!(
            "[RTC] Initial hardware RTC time: {}",
            defmt::Display2Format(&now.to_zoned(TIMEZONE))
        );
    }

    critical_section::with(|cs| {
        *RTC.borrow(cs).borrow_mut() = Some(rtc);
    });
}

/// Synchronizes the hardware RTC with UTC epoch seconds and microsecond fraction received over UART.
pub fn sync_rtc(epoch_seconds: u64, subsec_micros: u32) {
    critical_section::with(|cs| {
        let guard = RTC.borrow(cs);
        if let Some(ref rtc) = *guard.borrow() {
            // Read and log RTC time before update
            let before_us = rtc.current_time_us();
            if let Ok(before) = jiff::Timestamp::from_microsecond(before_us as i64) {
                info!(
                    "[RTC] RTC time before sync: {}",
                    defmt::Display2Format(&before.to_zoned(TIMEZONE))
                );
            }

            let total_us =
                epoch_seconds.saturating_mul(1_000_000).saturating_add(subsec_micros as u64);
            rtc.set_current_time_us(total_us);

            let received = jiff::Timestamp::from_microsecond(total_us as i64).ok();
            let after_us = rtc.current_time_us();
            let after = jiff::Timestamp::from_microsecond(after_us as i64).ok();

            if let (Some(rec), Some(aft)) = (received, after) {
                info!(
                    "[RTC] Received: {}\n[RTC] RTC time after sync: {}",
                    defmt::Display2Format(&rec.to_zoned(TIMEZONE)),
                    defmt::Display2Format(&aft.to_zoned(TIMEZONE))
                );
            }
        }
    });
}

/// Returns the current UTC wall-clock time as a `jiff::Timestamp`.
pub fn current_timestamp() -> Option<jiff::Timestamp> {
    critical_section::with(|cs| {
        let guard = RTC.borrow(cs);
        let rtc_ref = guard.borrow();
        let rtc = rtc_ref.as_ref()?;
        let us = rtc.current_time_us();
        // Ignore uninitialized RTC timestamps (e.g. before year 2024: 1,700,000,000s)
        if us < 1_700_000_000 * 1_000_000 {
            return None;
        }

        jiff::Timestamp::from_microsecond(us as i64).ok()
    })
}

/// Returns the current local wall-clock time configured for this device as a `jiff::Zoned`.
pub fn current_zoned() -> Option<jiff::Zoned> {
    current_timestamp().map(|ts| ts.to_zoned(TIMEZONE))
}
