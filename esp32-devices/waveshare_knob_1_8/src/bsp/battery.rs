// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Battery voltage monitoring via ADC1_CH0 on GPIO1.
//!
//! The Waveshare ESP32-S3-Knob-Touch-LCD-1.8 features a 1:1 voltage divider
//! (R62 = 10k, R63 = 10k) connected to GPIO1 (ADC1 Channel 0).
//! V_bat = 2 * V_adc.
//! Fully charged LiPo is ~4.2V (2.1V at ADC pin).
//! Depleted LiPo is ~3.3V (1.65V at ADC pin).
//! When USB is connected, VBUS (5.0V - 5.2V) powers the rail (above PLUGGED_MV at the pin, doubled).

use alloc::rc::Rc;

use coprocessor::proto::BatteryStatusT;
use defmt::{info, trace, warn};
use embassy_time::{Duration, Instant, Ticker, Timer};
use esp_hal::Blocking;
use esp_hal::analog::adc::{Adc, AdcCalCurve, AdcConfig, AdcPin, Attenuation};
use esp_hal::peripherals::{ADC1, GPIO1};

/// Time between reports. Samples are spread over it, so an estimate covers
/// the load swings in between (display, haptics), not a moment.
const REPORT_PERIOD: Duration = Duration::from_secs(10);
/// Samples per report: one every 100 ms.
const SAMPLES_PER_REPORT: usize = 100;
/// The first report's samples, so the menu has a value soon after boot.
const FIRST_REPORT_SAMPLES: usize = 10;
/// Above this, USB powers the rail: the battery's charge can't be read.
const PLUGGED_MV: u32 = 4300;
/// At or below this, on battery, the haptics pulse a warning.
const LOW_BATTERY_PERCENT: i32 = 10;
const LOW_BATTERY_PULSE_PERIOD: Duration = Duration::from_secs(30);

/// Converts raw battery millivolts to percentage [0..=100] using KrX3D piecewise calibration curve.
fn millivolts_to_percent(mv: u32) -> i32 {
    const CURVE: &[(u32, i32)] = &[
        (3000, 0),
        (3300, 5),
        (3500, 10),
        (3600, 20),
        (3650, 30),
        (3700, 40),
        (3750, 50),
        (3800, 60),
        (3880, 70),
        (3950, 80),
        (4000, 90),
        (4030, 95),
        (4050, 100),
    ];

    if mv <= CURVE[0].0 {
        return 0;
    }
    if mv >= CURVE[CURVE.len() - 1].0 {
        return 100;
    }

    for i in 0..CURVE.len() - 1 {
        let (v0, p0) = CURVE[i];
        let (v1, p1) = CURVE[i + 1];
        if mv >= v0 && mv <= v1 {
            let span_v = (v1 - v0) as i32;
            let span_p = p1 - p0;
            return p0 + (((mv - v0) as i32 * span_p) / span_v);
        }
    }
    100
}

/// The battery's ADC input: one calibrated reading at a time.
struct BatteryAdc {
    adc: Adc<'static, ADC1<'static>, Blocking>,
    pin: AdcPin<GPIO1<'static>, ADC1<'static>, AdcCalCurve<ADC1<'static>>>,
}

impl BatteryAdc {
    fn new(adc: ADC1<'static>, pin: GPIO1<'static>) -> Self {
        let mut config = AdcConfig::new();
        let pin = config.enable_pin_with_cal::<_, AdcCalCurve<ADC1>>(pin, Attenuation::_11dB);
        Self { adc: Adc::new(adc, config), pin }
    }

    /// Millivolts at the pin, or None if the conversion didn't finish.
    async fn read_mv(&mut self) -> Option<u16> {
        // A conversion takes a few tens of microseconds.
        for _ in 0..10 {
            match self.adc.read_oneshot(&mut self.pin) {
                Ok(mv) => return Some(mv),
                Err(nb::Error::WouldBlock) => Timer::after_micros(20).await,
                Err(nb::Error::Other(())) => break,
            }
        }
        None
    }
}

/// The battery is twice the pin: a 1:1 divider (R62, R63).
fn battery_mv(pin_mv: u32) -> u32 {
    2 * pin_mv
}

/// Turns a window of samples into one value.
trait Estimator {
    fn add(&mut self, sample: u16);
    /// The estimate of the samples added since the last take, if any. Starts
    /// a new window.
    fn take(&mut self) -> Option<u32>;
}

#[derive(Default)]
struct Mean {
    sum: u32,
    count: u32,
}

impl Estimator for Mean {
    fn add(&mut self, sample: u16) {
        self.sum += u32::from(sample);
        self.count += 1;
    }

    fn take(&mut self) -> Option<u32> {
        let mean = (self.count > 0).then(|| self.sum / self.count);
        *self = Self::default();
        mean
    }
}

/// Robust to the spikes a mean takes in.
#[derive(Default)]
struct Median {
    samples: heapless::Vec<u16, SAMPLES_PER_REPORT>,
}

impl Estimator for Median {
    fn add(&mut self, sample: u16) {
        // A window never holds more than SAMPLES_PER_REPORT.
        let _ = self.samples.push(sample);
    }

    fn take(&mut self) -> Option<u32> {
        self.samples.sort_unstable();
        let median = self.samples.get(self.samples.len() / 2).map(|&mv| u32::from(mv));
        self.samples.clear();
        median
    }
}

/// Exponential moving average across reports, alpha = 1/4.
#[derive(Default)]
struct Ema(Option<u32>);

impl Ema {
    fn update(&mut self, mv: u32) -> u32 {
        let smoothed = self.0.map_or(mv, |prev| (prev * 3 + mv + 2) / 4);
        self.0 = Some(smoothed);
        smoothed
    }

    /// Forgets the history: across plugging in or out it would only slew.
    fn reset(&mut self) {
        self.0 = None;
    }
}

/// While on battery, the percent only goes down: the voltage recovers when
/// the load drops (the screen dims, animations stop), but the charge doesn't.
/// Plugging in starts over.
#[derive(Default)]
struct Monotonic(Option<i32>);

impl Monotonic {
    fn update(&mut self, battery: common::Battery) -> common::Battery {
        if battery.is_plugged {
            self.0 = None;
            return battery;
        }
        let percent = self.0.map_or(battery.percent, |last| last.min(battery.percent));
        self.0 = Some(percent);
        common::Battery { percent, ..battery }
    }
}

/// Asks for a haptic pulse every LOW_BATTERY_PULSE_PERIOD while on battery at
/// LOW_BATTERY_PERCENT or less, the first as soon as it gets there.
#[derive(Default)]
struct LowBatteryAlert {
    last_pulse: Option<Instant>,
}

impl LowBatteryAlert {
    /// Whether to pulse at `now`.
    fn update(&mut self, battery: common::Battery, now: Instant) -> bool {
        if battery.is_plugged || battery.percent > LOW_BATTERY_PERCENT {
            self.last_pulse = None;
            return false;
        }
        if self.last_pulse.is_some_and(|last| now - last < LOW_BATTERY_PULSE_PERIOD) {
            return false;
        }
        self.last_pulse = Some(now);
        true
    }
}

#[embassy_executor::task]
pub async fn battery_task(
    adc: ADC1<'static>,
    pin: GPIO1<'static>,
    feedback: Rc<common::FeedbackChannel>,
    system_client: crate::bsp::coprocessor::SystemClient,
    battery: Rc<common::BatteryWatch>,
) {
    let battery = battery.sender();
    info!("[BATTERY] Starting battery ADC task on GPIO1...");

    let mut adc = BatteryAdc::new(adc, pin);
    let mut median = Median::default();
    let mut mean = Mean::default();
    let mut median_ema = Ema::default();
    let mut mean_ema = Ema::default();
    let mut monotonic = Monotonic::default();
    let mut low_battery = LowBatteryAlert::default();
    let mut last: Option<common::Battery> = None;

    let mut ticker = Ticker::every(REPORT_PERIOD / SAMPLES_PER_REPORT as u32);
    loop {
        let samples = if last.is_some() { SAMPLES_PER_REPORT } else { FIRST_REPORT_SAMPLES };
        for _ in 0..samples {
            ticker.next().await;
            if let Some(mv) = adc.read_mv().await {
                median.add(mv);
                mean.add(mv);
            }
        }
        let (Some(median_mv), Some(mean_mv)) = (median.take(), mean.take()) else {
            warn!("[BATTERY] No ADC readings in {} attempts", samples);
            continue;
        };

        // The median is authoritative; the mean is logged for comparison.
        let median_mv = battery_mv(median_mv);
        let plugged = median_mv > PLUGGED_MV;
        if last.is_some_and(|last| last.is_plugged != plugged) {
            median_ema.reset();
            mean_ema.reset();
        }
        let mv = median_ema.update(median_mv);
        let mean_mv = mean_ema.update(battery_mv(mean_mv));
        trace!("[BATTERY] median={}mV mean={}mV", mv, mean_mv);

        let state = monotonic.update(common::Battery {
            percent: if plugged { -1 } else { millivolts_to_percent(mv) },
            is_plugged: plugged,
        });
        if last != Some(state) {
            info!(
                "[BATTERY] median={}mV mean={}mV, percent={}%, plugged={}",
                mv, mean_mv, state.percent, plugged
            );
            let mut status = BatteryStatusT::default();
            status.millivolts = mv;
            // 0xFF: unknown while plugged in.
            status.percent = if plugged { 0xFF } else { state.percent as u8 };
            status.is_plugged = plugged;
            let _ = system_client.report_battery(&status).await;
            battery.send(state);
            last = Some(state);
        }

        if low_battery.update(state, Instant::now()) {
            warn!("[BATTERY] Low battery ({}%), pulsing haptic alert", state.percent);
            let _ = feedback.try_send(app_shell::Feedback::Haptic);
        }
    }
}
