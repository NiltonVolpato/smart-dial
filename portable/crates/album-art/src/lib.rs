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
/// `size`×`size` (rounded up to a multiple of 8 for hardware/SIMD alignment)
/// and dims it.
pub fn decode(jpeg: &[u8], size: u32) -> Option<SharedPixelBuffer<Rgb8Pixel>> {
    if size == 0 {
        return None;
    }
    let dims = jpeg::dimensions(jpeg).ok()?;
    if dims.width == 0 || dims.height == 0 {
        return None;
    }

    let aligned_size = size.next_multiple_of(8);
    let (scale, clip) = cover_scale_and_clip(dims, aligned_size)?;
    let mut pixels = SharedPixelBuffer::<Rgb8Pixel>::new(aligned_size, aligned_size);
    let bytes = pixels.make_mut_bytes();
    jpeg::decode_scaled(jpeg, scale, clip, bytes).ok()?;
    dim_rgb8_in_place(bytes, BRIGHTNESS_PERCENT);
    Some(pixels)
}

/// Computes `(scale, clip)` parameters satisfying `esp_new_jpeg`'s constraints:
/// - Scale width & height must be multiples of 8, `<= src`, and `>= ceil(src / 8)`
/// - If non-square, `clip` is `Some(aligned_size × aligned_size)` with `clip <= scale`.
fn cover_scale_and_clip(
    src: jpeg::Dimensions,
    aligned_size: u32,
) -> Option<(jpeg::Dimensions, Option<jpeg::Dimensions>)> {
    let min_src = src.width.min(src.height);
    if aligned_size > min_src {
        return None;
    }

    let scale_w = if src.width == min_src {
        aligned_size
    } else {
        ((src.width as u64 * aligned_size as u64).div_ceil(min_src as u64) as u32)
            .next_multiple_of(8)
            .min(src.width)
    };
    let scale_h = if src.height == min_src {
        aligned_size
    } else {
        ((src.height as u64 * aligned_size as u64).div_ceil(min_src as u64) as u32)
            .next_multiple_of(8)
            .min(src.height)
    };

    if scale_w < src.width.div_ceil(8) || scale_h < src.height.div_ceil(8) {
        return None;
    }

    let scale = jpeg::Dimensions { width: scale_w, height: scale_h };
    let clip = if scale_w != aligned_size || scale_h != aligned_size {
        Some(jpeg::Dimensions { width: aligned_size, height: aligned_size })
    } else {
        None
    };
    Some((scale, clip))
}

/// Scales every byte in `buf` by `dim_percent / 100` in place.
pub fn dim_rgb8_in_place(buf: &mut [u8], dim_percent: u8) {
    let dim_scale = (dim_percent as u32 * 256) / 100;
    for b in buf.iter_mut() {
        *b = ((*b as u32 * dim_scale) >> 8) as u8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MOCK_JPEG: &[u8] = include_bytes!("../../../apps/sonos/assets/album-art-mock.jpg");

    #[test]
    fn test_dim_rgb8_in_place() {
        let mut buf = [255u8; 3];
        dim_rgb8_in_place(&mut buf, 65);
        // 255 * 0.65 = 165.75 -> integer arithmetic gives 165
        assert_eq!(buf, [165, 165, 165]);
    }

    #[test]
    fn test_decode_rounds_up_to_multiple_of_8() {
        let pixels_360 = decode(MOCK_JPEG, 360).expect("decode 360");
        assert_eq!(pixels_360.width(), 360);
        assert_eq!(pixels_360.height(), 360);

        let pixels_390 = decode(MOCK_JPEG, 390).expect("decode 390");
        assert_eq!(pixels_390.width(), 392);
        assert_eq!(pixels_390.height(), 392);
    }
}
