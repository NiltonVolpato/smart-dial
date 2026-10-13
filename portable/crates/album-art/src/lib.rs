// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Album art for the playback view: a JPEG decoded, cropped to a square,
//! resampled and dimmed, off the UI's core.
//!
//! The UI's core sends an [`ArtRequest`] with the JPEG, and gets an
//! [`ArtResult`] back with the pixels, to make an image of (cheap: no copy).
//! Whoever runs [`decode`] in between (Core 1 on the device, a thread on the
//! host) owns the channels' other ends.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use slint::{Rgb8Pixel, SharedPixelBuffer};

/// The brightness album art is shown at, so text reads over it.
const BRIGHTNESS_PERCENT: u8 = 65;

/// A JPEG to decode. `generation` names the track it's for, so a result for
/// one that's no longer shown can be told apart.
pub struct ArtRequest {
    pub generation: u32,
    pub jpeg: Vec<u8>,
    /// The square's side, in pixels.
    pub size: u32,
}

/// The pixels of an [`ArtRequest`]'s JPEG; none if it didn't decode.
pub struct ArtResult {
    pub generation: u32,
    pub pixels: Option<SharedPixelBuffer<Rgb8Pixel>>,
}

/// To the decoder. One at a time: a newer track's art waits for the older.
pub type ArtRequests = Channel<CriticalSectionRawMutex, ArtRequest, 1>;
/// From the decoder.
pub type ArtResults = Channel<CriticalSectionRawMutex, ArtResult, 1>;

/// The decoder's channels, as the UI's side holds them.
#[derive(Clone, Copy)]
pub struct Decoder {
    pub requests: &'static ArtRequests,
    pub results: &'static ArtResults,
}

/// What the decoder does with a request: the JPEG is freed before the
/// result exists.
pub fn process(request: ArtRequest) -> ArtResult {
    let pixels = decode(&request.jpeg, request.size);
    ArtResult { generation: request.generation, pixels }
}

/// Decodes `jpeg`, crops it to a square (cover fit), resamples it to
/// `size`×`size` and dims it. Takes a while (~100 ms+): not on the UI's core.
pub fn decode(jpeg: &[u8], size: u32) -> Option<SharedPixelBuffer<Rgb8Pixel>> {
    let dims = jpegdec::dimensions(jpeg).ok()?;
    let mut rgb = alloc::vec![0u8; (dims.width * dims.height * 3) as usize];
    jpegdec::decode_into(jpeg, &mut rgb).ok()?;
    let mut pixels = SharedPixelBuffer::<Rgb8Pixel>::new(size, size);
    resample_and_dim_rgb8(
        &rgb,
        dims.width,
        dims.height,
        pixels.make_mut_bytes(),
        size,
        size,
        BRIGHTNESS_PERCENT,
    );
    Some(pixels)
}

/// Resamples an RGB8 pixel slice of dimensions `(src_w, src_h)` to `(dst_w, dst_h)`
/// with a center-crop (cover fit) and in-flight dimming factor `dim_percent` (0..=100).
/// Writes directly into `dst` (which must be at least `dst_w * dst_h * 3` bytes).
fn resample_and_dim_rgb8(
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

#[cfg(test)]
mod tests {
    use super::*;

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
}
