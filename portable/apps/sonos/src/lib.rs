// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::string::String;
use core::any::Any;
use core::cell::RefCell;
use core::fmt::Write;

use slint::Model;

slint::include_modules!();

#[derive(Clone)]
pub struct SonosAppFactory {
    info: app_shell::AppInfo,
}

impl SonosAppFactory {
    pub fn new() -> Self {
        let app = SonosApp::new().expect("Failed to create SonosApp");
        let info = app.global::<SonosInfo>().get_info();
        Self { info }
    }
}

impl Default for SonosAppFactory {
    fn default() -> Self {
        Self::new()
    }
}

pub fn format_remaining_time(duration: u32, elapsed: u32) -> String {
    let mut s = String::new();
    if duration > elapsed {
        let remaining = duration - elapsed;
        let minutes = remaining / 60;
        let seconds = remaining % 60;
        let _ = write!(s, "-{minutes:02}:{seconds:02}");
    } else {
        let _ = write!(s, "--:--");
    }
    s
}

pub fn format_elapsed_time(elapsed: u32) -> String {
    let mut s = String::new();
    let minutes = elapsed / 60;
    let seconds = elapsed % 60;
    let _ = write!(s, "{minutes:02}:{seconds:02}");
    s
}

const MOCK_ALBUM_ART_JPEG: &[u8] = include_bytes!("../assets/album-art-mock.jpg");

/// Resamples an RGB8 pixel slice of dimensions `(src_w, src_h)` to `(dst_w, dst_h)`
/// with a center-crop (cover fit) and in-flight dimming factor `dim_percent` (0..=100).
/// Writes directly into `dst` (which must be at least `dst_w * dst_h * 3` bytes).
pub fn resample_and_dim_rgb8(
    src: &[u8],
    src_w: u32,
    src_h: u32,
    dst: &mut [u8],
    dst_w: u32,
    dst_h: u32,
    dim_percent: u8,
) {
    if src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0 {
        return;
    }
    let expected_dst_len = (dst_w as usize) * (dst_h as usize) * 3;
    if dst.len() < expected_dst_len {
        return;
    }

    let side = src_w.min(src_h);
    let crop_x_fp = (((src_w - side) as u64) << 16) / 2;
    let crop_y_fp = (((src_h - side) as u64) << 16) / 2;
    let step_x_fp = ((side as u64) << 16) / (dst_w as u64);
    let step_y_fp = ((side as u64) << 16) / (dst_h as u64);

    let dim_scale = (dim_percent as u32 * 256) / 100;

    for y in 0..dst_h {
        let v_fp = crop_y_fp + (y as u64 * step_y_fp);
        let y0 = ((v_fp >> 16) as usize).min((src_h - 1) as usize);
        let y1 = (y0 + 1).min((src_h - 1) as usize);
        let fy = ((v_fp >> 8) & 0xFF) as u32;

        let row_dst_offset = (y as usize * dst_w as usize) * 3;
        let row0_src_offset = y0 * (src_w as usize) * 3;
        let row1_src_offset = y1 * (src_w as usize) * 3;

        for x in 0..dst_w {
            let u_fp = crop_x_fp + (x as u64 * step_x_fp);
            let x0 = ((u_fp >> 16) as usize).min((src_w - 1) as usize);
            let x1 = (x0 + 1).min((src_w - 1) as usize);
            let fx = ((u_fp >> 8) & 0xFF) as u32;

            let p00_idx = row0_src_offset + x0 * 3;
            let p10_idx = row0_src_offset + x1 * 3;
            let p01_idx = row1_src_offset + x0 * 3;
            let p11_idx = row1_src_offset + x1 * 3;

            let dst_idx = row_dst_offset + (x as usize) * 3;

            for c in 0..3 {
                let p00 = src.get(p00_idx + c).copied().unwrap_or(0) as u32;
                let p10 = src.get(p10_idx + c).copied().unwrap_or(0) as u32;
                let p01 = src.get(p01_idx + c).copied().unwrap_or(0) as u32;
                let p11 = src.get(p11_idx + c).copied().unwrap_or(0) as u32;

                let top = p00 * (256 - fx) + p10 * fx;
                let bot = p01 * (256 - fx) + p11 * fx;
                let val = (top * (256 - fy) + bot * fy) >> 16;

                dst[dst_idx + c] = ((val * dim_scale) >> 8).min(255) as u8;
            }
        }
    }
}

/// Decodes an arbitrary JPEG byte slice and resamples it to `target_size` square with
/// 35% dimming (65% brightness) directly into a Slint RGB8 SharedPixelBuffer allocated in PSRAM.
pub fn decode_and_resample_album_art(jpeg_bytes: &[u8], target_size: u32) -> Option<slint::Image> {
    let dims = jpegdec::dimensions(jpeg_bytes).ok()?;

    let mut raw_rgb = alloc::vec![0u8; (dims.width * dims.height * 3) as usize];
    jpegdec::decode_into(jpeg_bytes, &mut raw_rgb).ok()?;

    let mut pixel_buffer =
        slint::SharedPixelBuffer::<slint::Rgb8Pixel>::new(target_size, target_size);
    resample_and_dim_rgb8(
        &raw_rgb,
        dims.width,
        dims.height,
        pixel_buffer.make_mut_bytes(),
        target_size,
        target_size,
        65,
    );

    Some(slint::Image::from_rgb8(pixel_buffer))
}

/// Loads and prepares the default bundled mock album art for the given screen size.
pub fn load_default_album_art(target_size: u32) -> Option<slint::Image> {
    decode_and_resample_album_art(MOCK_ALBUM_ART_JPEG, target_size)
}

/// Updates the displayed album art from dynamic JPEG bytes received at runtime.
pub fn update_album_art(app: &SonosApp, jpeg_bytes: &[u8]) -> bool {
    let target_size = (app.get_screen_size() / 1.0).max(1.0) as u32;
    if let Some(art) = decode_and_resample_album_art(jpeg_bytes, target_size) {
        app.set_album_art(art);
        true
    } else {
        false
    }
}

struct MockTrack {
    title: &'static str,
    artist: &'static str,
    album: &'static str,
    duration: u32,
}

const MOCK_PLAYLIST: [MockTrack; 3] = [
    MockTrack {
        title: "NEVER NEVER (Extended Club Remaster)",
        artist: "Marc Moon",
        album: "Marc Moon EP",
        duration: 180,
    },
    MockTrack {
        title: "Solar Drift",
        artist: "Stellar Echo",
        album: "Cosmic Horizon",
        duration: 215,
    },
    MockTrack {
        title: "Midnight City",
        artist: "M83",
        album: "Hurry Up, We're Dreaming",
        duration: 243,
    },
];

/// Sets up interactive mock handlers and timers for the Sonos controller.
///
/// Returns a tuple of handles that must remain alive for the duration of the app session.
pub fn setup_sonos(app: &SonosApp) -> (Rc<RefCell<slint::Timer>>, slint::Timer) {
    // 0. Initialize album art in PSRAM sized to match screen dimensions
    let target_size = (app.get_screen_size() / 1.0).max(1.0) as u32;
    if let Some(art) = load_default_album_art(target_size) {
        app.set_album_art(art);
    }
    // 1. Play / Pause toggle
    let weak_play = app.as_weak();
    app.on_toggle_play_pause(move || {
        if let Some(app) = weak_play.upgrade() {
            let mut state = app.get_state();
            state.is_playing = !state.is_playing;
            app.set_state(state);
            app_shell::feedback::signal(app_shell::Feedback::Click);
        }
    });

    // 2. Volume adjustment with HUD auto-dismiss
    let weak_vol = app.as_weak();
    let volume_timer = Rc::new(RefCell::new(slint::Timer::default()));
    let v_timer_cb = volume_timer.clone();
    app.on_adjust_volume(move |delta| {
        if let Some(app) = weak_vol.upgrade() {
            let mut state = app.get_state();
            state.volume = (state.volume + delta).clamp(0, 100);
            state.volume_visible = true;
            app.set_state(state);
            app_shell::feedback::signal(app_shell::Feedback::DialStepForward);

            let weak_timer = app.as_weak();
            v_timer_cb.borrow_mut().start(
                slint::TimerMode::SingleShot,
                core::time::Duration::from_millis(1500),
                move || {
                    if let Some(app) = weak_timer.upgrade() {
                        let mut s = app.get_state();
                        s.volume_visible = false;
                        app.set_state(s);
                    }
                },
            );
        }
    });

    // 3. Group selection
    let weak_sel = app.as_weak();
    app.on_select_group(move |idx| {
        if let Some(app) = weak_sel.upgrade() {
            let mut state = app.get_state();
            let total = state.groups.row_count() as i32;
            if total > 0 {
                state.selected_group = idx.clamp(0, total - 1);
                if let Some(group) = state.groups.row_data(state.selected_group as usize) {
                    state.active_group_name = group.name;
                }
            }
            app.set_state(state);
            app_shell::feedback::signal(app_shell::Feedback::Click);
        }
    });

    // 4. Mode transition (0 = Group Selector, 1 = Now Playing)
    let weak_mode = app.as_weak();
    app.on_set_mode(move |mode| {
        if let Some(app) = weak_mode.upgrade() {
            let mut state = app.get_state();
            state.mode = mode;
            app.set_state(state);
            app_shell::feedback::signal(app_shell::Feedback::Click);
        }
    });

    // 5. Toggle time display (remaining vs elapsed) feedback
    app.on_toggle_time_display(|| {
        app_shell::feedback::signal(app_shell::Feedback::Click);
    });

    // 6. Track skip callbacks with mock playlist advancement
    let track_idx = Rc::new(RefCell::new(0usize));
    let elapsed_tracker = Rc::new(RefCell::new(45u32));

    let weak_next = app.as_weak();
    let track_idx_next = track_idx.clone();
    let elapsed_tracker_next = elapsed_tracker.clone();
    app.on_next_track(move || {
        if let Some(app) = weak_next.upgrade() {
            let mut idx = track_idx_next.borrow_mut();
            *idx = (*idx + 1) % MOCK_PLAYLIST.len();
            let track = &MOCK_PLAYLIST[*idx];
            *elapsed_tracker_next.borrow_mut() = 0;

            let mut state = app.get_state();
            state.track_title = track.title.into();
            state.track_artist = track.artist.into();
            state.track_album = track.album.into();
            state.progress_ratio = 0.0;
            state.elapsed_str = format_elapsed_time(0).as_str().into();
            state.remaining_str = format_remaining_time(track.duration, 0).as_str().into();
            app.set_state(state);
            app_shell::feedback::signal(app_shell::Feedback::Click);
        }
    });

    let weak_prev = app.as_weak();
    let track_idx_prev = track_idx.clone();
    let elapsed_tracker_prev = elapsed_tracker.clone();
    app.on_previous_track(move || {
        if let Some(app) = weak_prev.upgrade() {
            let mut idx = track_idx_prev.borrow_mut();
            *idx = if *idx == 0 { MOCK_PLAYLIST.len() - 1 } else { *idx - 1 };
            let track = &MOCK_PLAYLIST[*idx];
            *elapsed_tracker_prev.borrow_mut() = 0;

            let mut state = app.get_state();
            state.track_title = track.title.into();
            state.track_artist = track.artist.into();
            state.track_album = track.album.into();
            state.progress_ratio = 0.0;
            state.elapsed_str = format_elapsed_time(0).as_str().into();
            state.remaining_str = format_remaining_time(track.duration, 0).as_str().into();
            app.set_state(state);
            app_shell::feedback::signal(app_shell::Feedback::Click);
        }
    });

    // 7. Track progress simulation timer (1-second tick)
    let progress_timer = slint::Timer::default();
    let weak_progress = app.as_weak();
    let track_idx_prog = track_idx;
    let elapsed_tracker_prog = elapsed_tracker;
    progress_timer.start(
        slint::TimerMode::Repeated,
        core::time::Duration::from_secs(1),
        move || {
            if let Some(app) = weak_progress.upgrade() {
                let mut state = app.get_state();
                if state.is_playing {
                    let cur_idx = *track_idx_prog.borrow();
                    let duration = MOCK_PLAYLIST[cur_idx].duration;
                    let mut el = elapsed_tracker_prog.borrow_mut();
                    *el = (*el + 1) % duration;
                    state.progress_ratio = (*el as f32) / (duration as f32);
                    state.remaining_str = format_remaining_time(duration, *el).as_str().into();
                    state.elapsed_str = format_elapsed_time(*el).as_str().into();
                    app.set_state(state);
                }
            }
        },
    );

    (volume_timer, progress_timer)
}

impl app_shell::AppFactory for SonosAppFactory {
    fn info(&self) -> app_shell::AppInfo {
        self.info.clone()
    }

    fn launch(&self, context: app_shell::ShellContext) -> Box<dyn Any> {
        let app = SonosApp::new().expect("Failed to create SonosApp");
        let handles = setup_sonos(&app);

        let ctx = context.clone();
        theme::setup_navigation(
            &app,
            move || ctx.exit(),
            || {
                app_shell::feedback::signal(app_shell::Feedback::Click);
            },
        );

        // In Mode 1, edge back and edge quit return to Mode 0 (Groups)
        let nav = theme::Navigation::get(&app);
        let weak_nav_back = app.as_weak();
        nav.on_back(move || {
            if let Some(app) = weak_nav_back.upgrade() {
                let mut state = app.get_state();
                if state.mode == 1 {
                    state.mode = 0;
                    app.set_state(state);
                    app_shell::feedback::signal(app_shell::Feedback::Click);
                    return true;
                }
            }
            false
        });
        let weak_nav_quit = app.as_weak();
        nav.on_quit(move || {
            if let Some(app) = weak_nav_quit.upgrade() {
                let mut state = app.get_state();
                if state.mode == 1 {
                    state.mode = 0;
                    app.set_state(state);
                    app_shell::feedback::signal(app_shell::Feedback::Click);
                    return true;
                }
            }
            false
        });

        app.on_exit(move || context.exit());
        let _ = app.show();

        Box::new((app, handles))
    }

    fn clone_box(&self) -> Box<dyn app_shell::AppFactory> {
        Box::new(self.clone())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_format_remaining_time() {
        assert_eq!(format_remaining_time(180, 45), "-02:15");
        assert_eq!(format_remaining_time(180, 0), "-03:00");
        assert_eq!(format_remaining_time(60, 59), "-00:01");
        assert_eq!(format_remaining_time(60, 60), "--:--");
        assert_eq!(format_remaining_time(60, 70), "--:--");
    }

    #[test]
    fn test_format_elapsed_time() {
        assert_eq!(format_elapsed_time(0), "00:00");
        assert_eq!(format_elapsed_time(45), "00:45");
        assert_eq!(format_elapsed_time(65), "01:05");
        assert_eq!(format_elapsed_time(3600), "60:00");
    }

    #[test]
    fn test_resample_and_dim_rgb8() {
        let src = [255u8; 12]; // 2x2 white RGB
        let mut dst = [0u8; 3]; // 1x1 output
        resample_and_dim_rgb8(&src, 2, 2, &mut dst, 1, 1, 65);
        // 255 * 0.65 = 165.75 -> integer arithmetic gives 165
        assert_eq!(dst[0], 165);
        assert_eq!(dst[1], 165);
        assert_eq!(dst[2], 165);
    }

    #[test]
    fn test_load_default_album_art_multi_device() {
        let art_360 = load_default_album_art(360);
        assert!(art_360.is_some());
        let img_360 = art_360.unwrap();
        assert_eq!(img_360.size().width, 360);
        assert_eq!(img_360.size().height, 360);

        let art_390 = load_default_album_art(390);
        assert!(art_390.is_some());
        let img_390 = art_390.unwrap();
        assert_eq!(img_390.size().width, 390);
        assert_eq!(img_390.size().height, 390);
    }
}
