// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Statistical sampling profiler task for Xtensa LX7 (ESP32-S3).

extern crate alloc;

use core::cell::RefCell;
use core::num::NonZeroU8;
use core::sync::atomic::{AtomicU8, Ordering};

use allocator_api2::boxed::Box;
use app_shell::SampleTable;
use critical_section::Mutex;
use defmt::info;
use embassy_time::{Duration, Timer};
use esp_hal::peripherals::TIMG1;
use esp_hal::timer::PeriodicTimer;
use esp_hal::timer::timg::TimerGroup;
use esp_hal::{Blocking, handler};

use crate::channels::subscribe_user_activity;

/// Sampling frequency: 2 kHz (500 µs interval).
pub const SAMPLING_PERIOD: esp_hal::time::Duration = esp_hal::time::Duration::from_micros(500);

/// Evaluates whether the profiler is enabled based on the environment string.
pub const fn is_profiler_enabled(env: Option<&str>) -> bool {
    let Some(s) = env else { return false };
    let bytes = s.as_bytes();
    !matches!(bytes, b"" | b"0" | b"false" | b"no")
}

/// Parses a target scope number from `PROFILE` (e.g. `scope:8` or `scope=8`).
pub const fn parse_target_scope(env: Option<&str>) -> Option<NonZeroU8> {
    let Some(s) = env else { return None };
    let bytes = s.as_bytes();
    let n = bytes.len();
    let k = b"scope".len();
    let mut i = 0;
    while i + k < n {
        let matched = matches!(bytes.split_at(i).1.split_at(k + 1).0, b"scope:" | b"scope=");
        if matched {
            let bytes = bytes.split_at(i + k + 1).1;
            let n = if bytes.len() > 3 { 3 } else { bytes.len() };
            let mut val: u16 = 0;
            let mut k = 0;
            while k < n && bytes[k] >= b'0' && bytes[k] <= b'9' {
                val = val * 10 + (bytes[k] - b'0') as u16;
                k += 1;
            }
            if k > 0 && val > 0 && val <= 255 {
                return NonZeroU8::new(val as u8);
            }
        }
        i += 1;
    }
    None
}

#[cfg(not(test))]
pub const PROFILER_ENABLED: bool = is_profiler_enabled(option_env!("PROFILE"));
#[cfg(test)]
pub const PROFILER_ENABLED: bool = true;

#[cfg(not(test))]
pub const TARGET_SCOPE: Option<NonZeroU8> = parse_target_scope(option_env!("PROFILE"));
#[cfg(test)]
pub const TARGET_SCOPE: Option<NonZeroU8> = None;

static CURRENT_SCOPE: AtomicU8 = AtomicU8::new(0);

static TIMER: Mutex<RefCell<Option<PeriodicTimer<'static, Blocking>>>> =
    Mutex::new(RefCell::new(None));
static SAMPLES: Mutex<RefCell<Option<Box<SampleTable<512>, esp_alloc::InternalMemory>>>> =
    Mutex::new(RefCell::new(None));

/// RAII guard that restricts sampling to the lifetime of this scope.
pub struct ProfileScope {
    prev: u8,
    id: NonZeroU8,
}

impl ProfileScope {
    #[inline(always)]
    pub fn enter(id: NonZeroU8) -> Self {
        if !PROFILER_ENABLED {
            return Self { prev: 0, id };
        }
        let prev = CURRENT_SCOPE.swap(id.get(), Ordering::Relaxed);
        Self { prev, id }
    }
}

impl Drop for ProfileScope {
    #[inline(always)]
    fn drop(&mut self) {
        if !PROFILER_ENABLED {
            return;
        }
        let current = CURRENT_SCOPE.swap(self.prev, Ordering::Relaxed);
        if current != self.id.get() {
            defmt::error!("Scope mismatch on drop: expected {}, got {}", self.id.get(), current);
        }
    }
}

/// Marks the current lexical scope as an area of interest for profiling.
#[inline(always)]
#[must_use]
pub fn scope(id: NonZeroU8) -> ProfileScope {
    ProfileScope::enter(id)
}

/// Returns the currently active scope (0 = unscoped).
#[inline(always)]
pub fn current_scope() -> u8 {
    CURRENT_SCOPE.load(Ordering::Relaxed)
}

const RA_OFFSET: u32 = 3;

/// Checks if an address is a valid 16-byte aligned stack pointer in ESP32-S3 internal DRAM.
#[inline(always)]
fn is_valid_stack_pointer(sp: u32) -> bool {
    (sp & 0xF) == 0 && (0x3FC8_8010..=0x3FD0_0000).contains(&sp)
}

/// Attempts to sample the call site (the `call` instruction in the caller) of the interrupted frame.
#[cfg(target_arch = "xtensa")]
#[inline(never)]
pub fn sample_callsite(epc1: u32) -> Option<u32> {
    let current_sp: u32;

    unsafe {
        // Spill live window registers to memory so caller save areas are updated
        core::arch::asm!(
            "mov {0}, a1",
            "and a12, a12, a12", "rotw 3",
            "and a12, a12, a12", "rotw 3",
            "and a12, a12, a12", "rotw 3",
            "and a12, a12, a12", "rotw 3",
            "and a12, a12, a12", "rotw 4",
            out(reg) current_sp
        );

        let sanitized_epc1 = (epc1 & 0x3fff_ffff) | 0x4000_0000;
        let mut fp = current_sp;

        // Walk backwards up to 8 frames looking for the interrupt frame where ra == epc1
        for _ in 0..8 {
            if !is_valid_stack_pointer(fp) {
                return None;
            }

            let ra = ((fp - 16) as *const u32).read_volatile();
            let sanitized_ra = (ra & 0x3fff_ffff) | 0x4000_0000;
            let next_fp = ((fp - 12) as *const u32).read_volatile();

            if sanitized_ra == sanitized_epc1 {
                // Found the exception frame! next_fp is the interrupted function's SP.
                if !is_valid_stack_pointer(next_fp) {
                    return None;
                }

                let caller_ra = ((next_fp - 16) as *const u32).read_volatile();
                let sanitized_caller_ra = (caller_ra & 0x3fff_ffff) | 0x4000_0000;

                if sanitized_caller_ra <= 0x4000_0000 + RA_OFFSET {
                    return None;
                }

                return Some(sanitized_caller_ra - RA_OFFSET);
            }

            fp = next_fp;
        }

        None
    }
}

#[handler]
fn profiler_isr() {
    critical_section::with(|cs| {
        if let Some(timer) = TIMER.borrow_ref_mut(cs).as_mut() {
            timer.clear_interrupt();
        }
    });

    if let Some(target) = TARGET_SCOPE
        && CURRENT_SCOPE.load(Ordering::Relaxed) != target.get()
    {
        return;
    }

    #[cfg(target_arch = "xtensa")]
    let pc: u32 = {
        let epc1: u32;
        unsafe {
            core::arch::asm!("rsr.epc1 {0}", out(reg) epc1);
        }
        sample_callsite(epc1).unwrap_or(epc1)
    };

    #[cfg(not(target_arch = "xtensa"))]
    let pc: u32 = 0;

    critical_section::with(|cs| {
        if let Some(samples) = SAMPLES.borrow_ref_mut(cs).as_mut() {
            samples.record(pc);
        }
    });
}

/// Event or condition that triggers the start of sampling.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum ProfilerTrigger {
    /// Starts immediately upon task startup / boot.
    Boot,
    /// Starts upon the first user interaction (touch, dial, button).
    FirstInput,
    /// Starts after a specific duration delay from boot.
    AfterDelay(Duration),
}

/// Profiler configuration parsed from compile-time environment variable.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct ProfilerConfig {
    pub enabled: bool,
    pub trigger: ProfilerTrigger,
    pub duration: Duration,
}

impl ProfilerConfig {
    pub const fn disabled() -> Self {
        Self {
            enabled: false,
            trigger: ProfilerTrigger::FirstInput,
            duration: Duration::from_secs(10),
        }
    }

    /// Parses a configuration string (e.g. from `option_env!("PROFILE")`).
    pub fn parse(raw: Option<&str>) -> Self {
        let Some(raw) = raw else {
            return Self::disabled();
        };
        let trimmed = raw.trim();
        if trimmed.is_empty() || trimmed == "0" || trimmed == "false" || trimmed == "no" {
            return Self::disabled();
        }

        let mut trigger = ProfilerTrigger::FirstInput;
        let mut duration = Duration::from_secs(10);

        for part in trimmed.split(',') {
            let part = part.trim();
            if part.is_empty() || part.starts_with("scope:") || part.starts_with("scope=") {
                continue;
            }
            if part == "boot" || part == "0s" {
                trigger = ProfilerTrigger::Boot;
            } else if part == "input" {
                trigger = ProfilerTrigger::FirstInput;
            } else if let Some(delay_str) = part.strip_prefix("delay:") {
                if let Some(dur) = parse_duration(delay_str) {
                    trigger = ProfilerTrigger::AfterDelay(dur);
                }
            } else if let Some(dur) = parse_duration(part) {
                duration = dur;
            }
        }

        Self { enabled: true, trigger, duration }
    }
}

fn parse_duration(s: &str) -> Option<Duration> {
    let s = s.trim();
    if let Some(num_str) = s.strip_suffix("ms") {
        if let Ok(ms) = num_str.parse::<u64>() {
            return Some(Duration::from_millis(ms));
        }
    } else if let Some(num_str) = s.strip_suffix('s')
        && let Ok(secs) = num_str.parse::<u64>()
    {
        return Some(Duration::from_secs(secs));
    }
    None
}

/// Asynchronous Embassy background task owning the profiler timer and sampling lifecycle.
#[embassy_executor::task]
pub async fn profiler_task(timg1: TIMG1<'static>) {
    if !PROFILER_ENABLED {
        defmt::debug!("Profiler disabled (PROFILE unset).");
        return;
    }

    let config = ProfilerConfig::parse(option_env!("PROFILE"));
    defmt::info!(
        "Profiler armed: trigger={:?}, duration={:?}, target_scope={:?}",
        config.trigger,
        config.duration,
        TARGET_SCOPE
    );

    // 1. Wait for trigger condition
    match config.trigger {
        ProfilerTrigger::Boot => {}
        ProfilerTrigger::FirstInput => {
            if let Some(mut activity) = subscribe_user_activity() {
                activity.get().await;
                defmt::info!("Profiler triggered on first user input.");
            }
        }
        ProfilerTrigger::AfterDelay(delay) => {
            Timer::after(delay).await;
            defmt::info!("Profiler delay elapsed, starting capture.");
        }
    }

    // 2. Allocate sample table in the heap
    critical_section::with(|cs| {
        let table = Box::new_in(SampleTable::<512>::new(), esp_alloc::InternalMemory);
        SAMPLES.borrow_ref_mut(cs).replace(table);
    });

    // 3. Initialize hardware timer and ISR
    let timg = TimerGroup::new(timg1);
    let mut timer = PeriodicTimer::new(timg.timer0);
    timer.set_interrupt_handler(profiler_isr);
    timer.listen();
    if let Err(e) = timer.start(SAMPLING_PERIOD) {
        defmt::error!("Failed to start profiler timer: {:?}", defmt::Debug2Format(&e));
        critical_section::with(|cs| {
            let _ = SAMPLES.borrow_ref_mut(cs).take();
        });
        return;
    }

    critical_section::with(|cs| {
        TIMER.borrow_ref_mut(cs).replace(timer);
    });

    defmt::info!("Profiler sampling started at 2 kHz for {:?}", config.duration);

    // 4. Await capture duration
    Timer::after(config.duration).await;

    // 5. Stop sampling and silence hardware timer immediately
    critical_section::with(|cs| {
        if let Some(mut timer) = TIMER.borrow_ref_mut(cs).take() {
            timer.unlisten();
        }
    });
    defmt::info!("Profiler sampling stopped. Timer unlistened.");

    // 6. Extract samples table, dump hotspot report, and drop from heap
    let table = critical_section::with(|cs| SAMPLES.borrow_ref_mut(cs).take());
    if let Some(table) = table {
        dump_report(&table);
    }
}

fn dump_report(table: &SampleTable<512>) {
    let total = table.total_samples();
    let unique = table.unique_count();

    info!("================================================================");
    info!("[PROFILE] Sampling Profiling Complete!");
    info!("[PROFILE] Total samples: {} (across {} unique PCs)", total, unique);

    if total > 0 {
        let (top, count) = table.top_samples::<15>();
        info!("[PROFILE] Top {} Hotspots:", count);
        for (i, entry) in top[..count].iter().enumerate() {
            let pct = (entry.count as f32 / total as f32) * 100.0;
            info!(
                "[PROFILE]   #{}: 0x{:08x} - {} samples ({}%)",
                i + 1,
                entry.pc,
                entry.count,
                pct
            );
        }
        info!("[PROFILE] To symbolize, run: python3 tools/symbolize_profile.py");
    } else {
        info!("[PROFILE] No samples captured during the sampling window.");
    }
    info!("================================================================");
    info!("{}", esp_alloc::HEAP.stats());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_is_profiler_enabled() {
        assert!(!is_profiler_enabled(None));
        assert!(!is_profiler_enabled(Some("")));
        assert!(!is_profiler_enabled(Some("0")));
        assert!(!is_profiler_enabled(Some("false")));
        assert!(!is_profiler_enabled(Some("no")));
        assert!(is_profiler_enabled(Some("1")));
        assert!(is_profiler_enabled(Some("true")));
        assert!(is_profiler_enabled(Some("boot,5s")));
        assert!(is_profiler_enabled(Some("scope:8")));
    }

    #[test]
    fn test_parse_target_scope() {
        assert_eq!(parse_target_scope(None), None);
        assert_eq!(parse_target_scope(Some("")), None);
        assert_eq!(parse_target_scope(Some("1")), None);
        assert_eq!(parse_target_scope(Some("boot,10s")), None);
        assert_eq!(parse_target_scope(Some("scope:0")), None);
        assert_eq!(parse_target_scope(Some("scope:8")), NonZeroU8::new(8));
        assert_eq!(parse_target_scope(Some("scope=42")), NonZeroU8::new(42));
        assert_eq!(parse_target_scope(Some("boot,scope:8,10s")), NonZeroU8::new(8));
        assert_eq!(parse_target_scope(Some("delay:2s,scope:255")), NonZeroU8::new(255));
        assert_eq!(parse_target_scope(Some("scope:256")), None);
    }

    #[test]
    fn test_profiler_config_disabled() {
        assert_eq!(ProfilerConfig::parse(None).enabled, false);
        assert_eq!(ProfilerConfig::parse(Some("")).enabled, false);
        assert_eq!(ProfilerConfig::parse(Some("0")).enabled, false);
        assert_eq!(ProfilerConfig::parse(Some("false")).enabled, false);
        assert_eq!(ProfilerConfig::parse(Some("no")).enabled, false);
    }

    #[test]
    fn test_profiler_config_defaults() {
        let c1 = ProfilerConfig::parse(Some("1"));
        assert_eq!(c1.enabled, true);
        assert_eq!(c1.trigger, ProfilerTrigger::FirstInput);
        assert_eq!(c1.duration, Duration::from_secs(10));

        let c2 = ProfilerConfig::parse(Some("true"));
        assert_eq!(c2.enabled, true);
        assert_eq!(c2.trigger, ProfilerTrigger::FirstInput);
        assert_eq!(c2.duration, Duration::from_secs(10));
    }

    #[test]
    fn test_profiler_config_custom_options() {
        let c1 = ProfilerConfig::parse(Some("boot,5s"));
        assert_eq!(c1.enabled, true);
        assert_eq!(c1.trigger, ProfilerTrigger::Boot);
        assert_eq!(c1.duration, Duration::from_secs(5));

        let c2 = ProfilerConfig::parse(Some("input,15s"));
        assert_eq!(c2.enabled, true);
        assert_eq!(c2.trigger, ProfilerTrigger::FirstInput);
        assert_eq!(c2.duration, Duration::from_secs(15));

        let c3 = ProfilerConfig::parse(Some("delay:2s,8s,scope:8"));
        assert_eq!(c3.enabled, true);
        assert_eq!(c3.trigger, ProfilerTrigger::AfterDelay(Duration::from_secs(2)));
        assert_eq!(c3.duration, Duration::from_secs(8));
    }

    #[test]
    fn test_scope_enter_and_drop() {
        CURRENT_SCOPE.store(0, Ordering::Relaxed);
        assert_eq!(current_scope(), 0);

        {
            let scope1 = scope(NonZeroU8::new(1).unwrap());
            assert_eq!(current_scope(), 1);

            {
                let _scope2 = scope(NonZeroU8::new(2).unwrap());
                assert_eq!(current_scope(), 2);
            }

            assert_eq!(current_scope(), 1);
            drop(scope1);
        }

        assert_eq!(current_scope(), 0);
    }
}
