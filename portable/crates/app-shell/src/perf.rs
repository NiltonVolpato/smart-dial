// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Performance metrics tracking and aggregation.

use core::time::Duration;

const CYCLES_PER_MS: u32 = 240_000;
const SCREEN_PIXELS: u32 = 390 * 390; // 152,100 pixels

/// Per-frame cycle metrics collected with hardware counter.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct FrameCycles {
    pub render_cycles: u32,
    pub transfer_cycles: u32,
    pub dirty_pixels: u32,
    pub rect_count: u16,
}

/// Rolling summary of performance metrics over a time window.
#[derive(Copy, Clone, Debug, Default, PartialEq)]
pub struct PerfSummary {
    pub frame_count: u32,
    pub fps: f32,
    pub avg_render_ms: f32,
    pub max_render_ms: f32,
    pub avg_transfer_ms: f32,
    pub max_transfer_ms: f32,
    pub avg_dirty_percent: f32,
    pub total_rects: u32,
}

impl defmt::Format for PerfSummary {
    fn format(&self, f: defmt::Formatter) {
        defmt::write!(
            f,
            "{=f32} FPS | render: avg {=f32}ms (max {=f32}ms) | transfer: avg {=f32}ms (max {=f32}ms) | dirty: {=f32}% ({} rects, {} frames)",
            self.fps,
            self.avg_render_ms,
            self.max_render_ms,
            self.avg_transfer_ms,
            self.max_transfer_ms,
            self.avg_dirty_percent,
            self.total_rects,
            self.frame_count
        )
    }
}

#[derive(Clone, Debug)]
pub struct PerfTracker {
    cycles_per_ms: u32,
    period: Duration,
    window_start: Duration,
    frame_count: u32,
    total_render_cycles: u64,
    max_render_cycles: u32,
    total_transfer_cycles: u64,
    max_transfer_cycles: u32,
    total_dirty_pixels: u64,
    total_rects: u32,
}

impl Default for PerfTracker {
    fn default() -> Self {
        Self::new()
    }
}

impl PerfTracker {
    pub fn new() -> Self {
        Self {
            cycles_per_ms: CYCLES_PER_MS,
            period: Duration::from_secs(1),
            window_start: Duration::ZERO,
            frame_count: 0,
            total_render_cycles: 0,
            max_render_cycles: 0,
            total_transfer_cycles: 0,
            max_transfer_cycles: 0,
            total_dirty_pixels: 0,
            total_rects: 0,
        }
    }

    pub fn with_period(mut self, period: Duration) -> Self {
        self.period = period;
        self
    }

    pub fn with_cycles_per_ms(mut self, cycles_per_ms: u32) -> Self {
        self.cycles_per_ms = cycles_per_ms.max(1);
        self
    }

    pub fn record_frame(&mut self, frame: FrameCycles) {
        self.frame_count += 1;
        self.total_render_cycles += frame.render_cycles as u64;
        self.max_render_cycles = self.max_render_cycles.max(frame.render_cycles);
        self.total_transfer_cycles += frame.transfer_cycles as u64;
        self.max_transfer_cycles = self.max_transfer_cycles.max(frame.transfer_cycles);
        self.total_dirty_pixels += frame.dirty_pixels as u64;
        self.total_rects += frame.rect_count as u32;
    }

    /// Evaluates if the reporting period has elapsed.
    /// If so and frames were drawn, returns `Some(PerfSummary)` and resets buckets.
    /// If period elapsed but no frames were drawn, resets the window start and returns `None`.
    pub fn take_summary(&mut self, current_time: Duration) -> Option<PerfSummary> {
        let elapsed = current_time.saturating_sub(self.window_start);
        if elapsed < self.period {
            return None;
        }

        self.window_start = current_time;

        if self.frame_count == 0 {
            return None;
        }

        let elapsed_secs = (elapsed.as_micros() as f32 / 1_000_000.0).max(0.001);
        let frames_f32 = self.frame_count as f32;
        let c_per_ms = self.cycles_per_ms as f32;

        let summary = PerfSummary {
            frame_count: self.frame_count,
            fps: frames_f32 / elapsed_secs,
            avg_render_ms: (self.total_render_cycles as f32 / frames_f32) / c_per_ms,
            max_render_ms: self.max_render_cycles as f32 / c_per_ms,
            avg_transfer_ms: (self.total_transfer_cycles as f32 / frames_f32) / c_per_ms,
            max_transfer_ms: self.max_transfer_cycles as f32 / c_per_ms,
            avg_dirty_percent: (self.total_dirty_pixels as f32
                / (SCREEN_PIXELS as f32 * frames_f32))
                * 100.0,
            total_rects: self.total_rects,
        };

        // Reset accumulation counters
        self.frame_count = 0;
        self.total_render_cycles = 0;
        self.max_render_cycles = 0;
        self.total_transfer_cycles = 0;
        self.max_transfer_cycles = 0;
        self.total_dirty_pixels = 0;
        self.total_rects = 0;

        Some(summary)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_initial_state() {
        let mut tracker = PerfTracker::new();
        // Under 1s: None
        assert_eq!(tracker.take_summary(Duration::from_millis(500)), None);
        // At 1s but 0 frames: None
        assert_eq!(tracker.take_summary(Duration::from_millis(1000)), None);
    }

    #[test]
    fn test_single_frame_summary() {
        let mut tracker = PerfTracker::new();

        // 10ms render = 2,400,000 cycles
        // 5ms transfer = 1,200,000 cycles
        // 152,100 pixels = 100%
        tracker.record_frame(FrameCycles {
            render_cycles: 2_400_000,
            transfer_cycles: 1_200_000,
            dirty_pixels: 152_100,
            rect_count: 2,
        });

        let summary = tracker.take_summary(Duration::from_secs(1)).expect("Expected summary");
        assert_eq!(summary.frame_count, 1);
        assert!((summary.fps - 1.0).abs() < 0.01);
        assert!((summary.avg_render_ms - 10.0).abs() < 0.01);
        assert!((summary.max_render_ms - 10.0).abs() < 0.01);
        assert!((summary.avg_transfer_ms - 5.0).abs() < 0.01);
        assert!((summary.max_transfer_ms - 5.0).abs() < 0.01);
        assert!((summary.avg_dirty_percent - 100.0).abs() < 0.01);
        assert_eq!(summary.total_rects, 2);

        // Next take_summary before period expires returns None
        assert_eq!(tracker.take_summary(Duration::from_millis(1500)), None);
    }

    #[test]
    fn test_multi_frame_averages() {
        let mut tracker = PerfTracker::new();

        // Frame 1: 10ms render (2.4M cycles), 5ms transfer (1.2M cycles), 50% dirty
        tracker.record_frame(FrameCycles {
            render_cycles: 2_400_000,
            transfer_cycles: 1_200_000,
            dirty_pixels: 76_050,
            rect_count: 1,
        });

        // Frame 2: 20ms render (4.8M cycles), 15ms transfer (3.6M cycles), 100% dirty
        tracker.record_frame(FrameCycles {
            render_cycles: 4_800_000,
            transfer_cycles: 3_600_000,
            dirty_pixels: 152_100,
            rect_count: 3,
        });

        let summary = tracker.take_summary(Duration::from_secs(1)).expect("Expected summary");
        assert_eq!(summary.frame_count, 2);
        assert!((summary.fps - 2.0).abs() < 0.01);
        assert!((summary.avg_render_ms - 15.0).abs() < 0.01);
        assert!((summary.max_render_ms - 20.0).abs() < 0.01);
        assert!((summary.avg_transfer_ms - 10.0).abs() < 0.01);
        assert!((summary.max_transfer_ms - 15.0).abs() < 0.01);
        assert!((summary.avg_dirty_percent - 75.0).abs() < 0.01);
        assert_eq!(summary.total_rects, 4);
    }

    #[test]
    fn test_idle_resets_window_without_reporting() {
        let mut tracker = PerfTracker::new();

        // 1s passes with no frames
        assert_eq!(tracker.take_summary(Duration::from_secs(1)), None);

        // Frame recorded at 1.5s
        tracker.record_frame(FrameCycles {
            render_cycles: 2_400_000,
            transfer_cycles: 1_200_000,
            dirty_pixels: 152_100,
            rect_count: 1,
        });

        // 1.8s (not yet 1s since last window at 1.0s)
        assert_eq!(tracker.take_summary(Duration::from_millis(1800)), None);

        // At 2.0s (1.0s elapsed since 1.0s window start)
        let summary = tracker.take_summary(Duration::from_secs(2)).expect("Expected summary");
        assert_eq!(summary.frame_count, 1);
    }
}
