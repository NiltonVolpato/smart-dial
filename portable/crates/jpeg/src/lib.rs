// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Portable baseline JPEG decoding into RGB8 buffers.
//!
//! Uses Espressif's `esp_new_jpeg` library (with PIE SIMD acceleration, hardware-aligned
//! scaling, and clipping) on Xtensa targets and `zune-jpeg` on host targets.

#![no_std]

extern crate alloc;

/// Errors that can occur during JPEG header parsing or decoding.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidData,
    UnsupportedFeature,
    DecodeError,
}

/// Decoded image dimensions in pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dimensions {
    pub width: u32,
    pub height: u32,
}

/// Returns the dimensions of the JPEG image without decoding the pixel data.
pub fn dimensions(jpeg_data: &[u8]) -> Result<Dimensions, Error> {
    imp::dimensions(jpeg_data)
}

/// Decodes a JPEG image into an RGB8 (`[R, G, B]`) buffer at its full resolution.
///
/// `output` must be at least `width * height * 3` bytes.
pub fn decode_into(jpeg_data: &[u8], output: &mut [u8]) -> Result<Dimensions, Error> {
    imp::decode_into(jpeg_data, output)
}

/// Decodes a JPEG image with integrated scaling and top-left clipping into an RGB8 buffer.
///
/// Both `scale` and `clip` dimensions must be multiples of 8, `clip` must be `<= scale`,
/// and the scale ratio must be `>= 1/8` of the source dimensions, matching `esp_new_jpeg`'s
/// decoder pipeline (`decode -> scale -> clipper`).
///
/// `output` must be at least `out_width * out_height * 3` bytes, where `(out_width, out_height)`
/// is `clip` if provided, or `scale` otherwise.
pub fn decode_scaled(
    jpeg_data: &[u8],
    scale: Dimensions,
    clip: Option<Dimensions>,
    output: &mut [u8],
) -> Result<Dimensions, Error> {
    imp::decode_scaled(jpeg_data, scale, clip, output)
}

#[cfg(target_arch = "xtensa")]
mod imp {
    use alloc::alloc::{Layout, alloc, dealloc};
    use core::ffi::{c_char, c_int, c_uint, c_void};
    use core::ptr;

    use super::{Dimensions, Error};

    const fn fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
        (a as u32) | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
    }

    const JPEG_PIXEL_FORMAT_RGB888: u32 = fourcc(b'R', b'G', b'B', b'3');
    const JPEG_ROTATE_0D: u32 = 0;
    const JPEG_ERR_OK: c_int = 0;
    const JPEG_ERR_UNSUPPORT_FMT: c_int = -6;
    const JPEG_ERR_UNSUPPORT_STD: c_int = -7;

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct JpegResolution {
        width: u16,
        height: u16,
    }

    #[repr(C)]
    struct JpegDecConfig {
        output_type: u32,
        scale: JpegResolution,
        clipper: JpegResolution,
        rotate: u32,
        block_enable: bool,
    }

    #[repr(C)]
    struct JpegDecHeaderInfo {
        width: u16,
        height: u16,
    }

    #[repr(C)]
    struct JpegDecIo {
        inbuf: *const u8,
        inbuf_len: c_int,
        inbuf_remain: c_int,
        outbuf: *mut u8,
        out_size: c_int,
    }

    type JpegDecHandle = *mut c_void;

    unsafe extern "C" {
        fn jpeg_dec_open(config: *const JpegDecConfig, jpeg_dec: *mut JpegDecHandle) -> c_int;
        fn jpeg_dec_parse_header(
            jpeg_dec: JpegDecHandle,
            io: *mut JpegDecIo,
            out_info: *mut JpegDecHeaderInfo,
        ) -> c_int;
        fn jpeg_dec_get_outbuf_len(jpeg_dec: JpegDecHandle, outbuf_len: *mut c_int) -> c_int;
        fn jpeg_dec_process(jpeg_dec: JpegDecHandle, io: *mut JpegDecIo) -> c_int;
        fn jpeg_dec_close(jpeg_dec: JpegDecHandle) -> c_int;
    }

    struct DecHandle(JpegDecHandle);

    impl DecHandle {
        fn open(config: &JpegDecConfig) -> Result<Self, Error> {
            let mut raw: JpegDecHandle = ptr::null_mut();
            let ret = unsafe { jpeg_dec_open(config, &mut raw) };
            if ret != JPEG_ERR_OK {
                assert!(raw.is_null());
                return Err(map_err(ret));
            }
            assert!(!raw.is_null());
            Ok(Self(raw))
        }
    }

    impl Drop for DecHandle {
        fn drop(&mut self) {
            unsafe {
                let _ = jpeg_dec_close(self.0);
            }
        }
    }

    fn map_err(code: c_int) -> Error {
        match code {
            JPEG_ERR_UNSUPPORT_FMT | JPEG_ERR_UNSUPPORT_STD => Error::UnsupportedFeature,
            _ => Error::DecodeError,
        }
    }

    pub fn dimensions(jpeg_data: &[u8]) -> Result<Dimensions, Error> {
        if jpeg_data.is_empty() {
            return Err(Error::InvalidData);
        }
        let config = JpegDecConfig {
            output_type: JPEG_PIXEL_FORMAT_RGB888,
            scale: JpegResolution { width: 0, height: 0 },
            clipper: JpegResolution { width: 0, height: 0 },
            rotate: JPEG_ROTATE_0D,
            block_enable: false,
        };
        let handle = DecHandle::open(&config)?;

        let mut io = JpegDecIo {
            inbuf: jpeg_data.as_ptr(),
            inbuf_len: jpeg_data.len() as c_int,
            inbuf_remain: 0,
            outbuf: ptr::null_mut(),
            out_size: 0,
        };
        let mut header = JpegDecHeaderInfo { width: 0, height: 0 };
        let ret = unsafe { jpeg_dec_parse_header(handle.0, &mut io, &mut header) };
        if ret != JPEG_ERR_OK || header.width == 0 || header.height == 0 {
            return Err(Error::InvalidData);
        }
        Ok(Dimensions { width: header.width as u32, height: header.height as u32 })
    }

    pub fn decode_into(jpeg_data: &[u8], output: &mut [u8]) -> Result<Dimensions, Error> {
        decode_with_config(
            jpeg_data,
            JpegResolution { width: 0, height: 0 },
            JpegResolution { width: 0, height: 0 },
            output,
        )
    }

    pub fn decode_scaled(
        jpeg_data: &[u8],
        scale: Dimensions,
        clip: Option<Dimensions>,
        output: &mut [u8],
    ) -> Result<Dimensions, Error> {
        let clip_res = match clip {
            Some(c) => JpegResolution { width: c.width as u16, height: c.height as u16 },
            None => JpegResolution { width: 0, height: 0 },
        };
        decode_with_config(
            jpeg_data,
            JpegResolution { width: scale.width as u16, height: scale.height as u16 },
            clip_res,
            output,
        )
    }

    fn decode_with_config(
        jpeg_data: &[u8],
        scale: JpegResolution,
        clipper: JpegResolution,
        output: &mut [u8],
    ) -> Result<Dimensions, Error> {
        if jpeg_data.is_empty() {
            return Err(Error::InvalidData);
        }
        let config = JpegDecConfig {
            output_type: JPEG_PIXEL_FORMAT_RGB888,
            scale,
            clipper,
            rotate: JPEG_ROTATE_0D,
            block_enable: false,
        };
        let handle = DecHandle::open(&config)?;

        let mut io = JpegDecIo {
            inbuf: jpeg_data.as_ptr(),
            inbuf_len: jpeg_data.len() as c_int,
            inbuf_remain: 0,
            outbuf: ptr::null_mut(),
            out_size: 0,
        };
        let mut header = JpegDecHeaderInfo { width: 0, height: 0 };
        let ret = unsafe { jpeg_dec_parse_header(handle.0, &mut io, &mut header) };
        if ret != JPEG_ERR_OK {
            return Err(map_err(ret));
        }

        let mut outbuf_len: c_int = 0;
        let ret = unsafe { jpeg_dec_get_outbuf_len(handle.0, &mut outbuf_len) };
        if ret != JPEG_ERR_OK || outbuf_len <= 0 {
            return Err(Error::DecodeError);
        }
        if output.len() < outbuf_len as usize || !(output.as_ptr() as usize).is_multiple_of(16) {
            return Err(Error::InvalidData);
        }

        io.outbuf = output.as_mut_ptr();
        let ret = unsafe { jpeg_dec_process(handle.0, &mut io) };
        if ret != JPEG_ERR_OK {
            return Err(map_err(ret));
        }

        Ok(Dimensions { width: header.width as u32, height: header.height as u32 })
    }

    // =========================================================================
    // Bare-metal ESP-IDF shims required by libesp_new_jpeg.a
    // =========================================================================

    const MALLOC_CAP_INTERNAL: u32 = 1 << 11;

    unsafe fn calloc_aligned(alignment: usize, size: usize, caps: u32) -> *mut c_void {
        assert!(alignment <= 16);
        let total_size = size + 16;
        let layout = unsafe { Layout::from_size_align_unchecked(total_size, 16) };
        let ptr = if (caps & MALLOC_CAP_INTERNAL) != 0 {
            unsafe {
                esp_alloc::HEAP.alloc_caps(esp_alloc::MemoryCapability::Internal.into(), layout)
            }
        } else {
            unsafe { alloc(layout) }
        };
        if ptr.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            ptr.write_bytes(0, total_size);
            *(ptr as *mut usize) = total_size;
            ptr.add(16).cast()
        }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn heap_caps_aligned_calloc(
        alignment: usize,
        n: usize,
        size: usize,
        caps: u32,
    ) -> *mut c_void {
        unsafe { calloc_aligned(alignment, n * size, caps) }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn heap_caps_calloc_prefer(
        n: usize,
        size: usize,
        _num: usize,
        caps1: u32,
        _caps2: u32,
    ) -> *mut c_void {
        unsafe { calloc_aligned(16, n * size, caps1) }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn heap_caps_free(ptr: *mut c_void) {
        if ptr.is_null() {
            return;
        }
        unsafe {
            let base = ptr.cast::<u8>().sub(16);
            let total_size = *(base as *const usize);
            dealloc(base, Layout::from_size_align_unchecked(total_size, 16));
        }
    }

    #[unsafe(no_mangle)]
    extern "C" fn esp_log_timestamp() -> c_uint {
        0
    }

    #[unsafe(no_mangle)]
    extern "C" fn esp_log_write(_level: c_int, _tag: *const c_char, _format: *const c_char) {}

    #[unsafe(no_mangle)]
    extern "C" fn ceil(x: f64) -> f64 {
        let trunc = (x as i64) as f64;
        if x > trunc { trunc + 1.0 } else { trunc }
    }
}

#[cfg(not(target_arch = "xtensa"))]
mod imp {
    use alloc::vec;

    use zune_jpeg::JpegDecoder;
    use zune_jpeg::zune_core::bytestream::ZCursor;
    use zune_jpeg::zune_core::colorspace::ColorSpace;
    use zune_jpeg::zune_core::options::DecoderOptions;

    use super::{Dimensions, Error};

    fn make_decoder(jpeg_data: &[u8]) -> JpegDecoder<ZCursor<&[u8]>> {
        let options = DecoderOptions::default().jpeg_set_out_colorspace(ColorSpace::RGB);
        JpegDecoder::new_with_options(ZCursor::new(jpeg_data), options)
    }

    pub fn dimensions(jpeg_data: &[u8]) -> Result<Dimensions, Error> {
        let mut decoder = make_decoder(jpeg_data);
        decoder.decode_headers().map_err(|_| Error::InvalidData)?;
        let (width, height) = decoder.dimensions().ok_or(Error::InvalidData)?;
        if width == 0 || height == 0 {
            return Err(Error::InvalidData);
        }
        Ok(Dimensions { width: width as u32, height: height as u32 })
    }

    pub fn decode_into(jpeg_data: &[u8], output: &mut [u8]) -> Result<Dimensions, Error> {
        let mut decoder = make_decoder(jpeg_data);
        decoder.decode_headers().map_err(|_| Error::InvalidData)?;
        let (width, height) = decoder.dimensions().ok_or(Error::InvalidData)?;
        if width == 0 || height == 0 {
            return Err(Error::InvalidData);
        }
        let expected = width * height * 3;
        if output.len() < expected {
            return Err(Error::InvalidData);
        }
        decoder.decode_into(&mut output[..expected]).map_err(|_| Error::DecodeError)?;
        Ok(Dimensions { width: width as u32, height: height as u32 })
    }

    pub fn decode_scaled(
        jpeg_data: &[u8],
        scale: Dimensions,
        clip: Option<Dimensions>,
        output: &mut [u8],
    ) -> Result<Dimensions, Error> {
        let src = dimensions(jpeg_data)?;
        if scale.width == 0
            || scale.height == 0
            || !scale.width.is_multiple_of(8)
            || !scale.height.is_multiple_of(8)
            || scale.width > src.width
            || scale.height > src.height
            || scale.width < src.width.div_ceil(8)
            || scale.height < src.height.div_ceil(8)
        {
            return Err(Error::InvalidData);
        }
        let out_dims = match clip {
            Some(c) => {
                if c.width == 0
                    || c.height == 0
                    || !c.width.is_multiple_of(8)
                    || !c.height.is_multiple_of(8)
                    || c.width > scale.width
                    || c.height > scale.height
                {
                    return Err(Error::InvalidData);
                }
                c
            }
            None => scale,
        };
        let expected = (out_dims.width as usize) * (out_dims.height as usize) * 3;
        if output.len() < expected {
            return Err(Error::InvalidData);
        }

        let mut full = vec![0u8; (src.width * src.height * 3) as usize];
        decode_into(jpeg_data, &mut full)?;

        // Scale from (src.width, src.height) -> (scale.width, scale.height) and clip top-left (out_dims.width, out_dims.height)
        let step_x_fp = ((src.width as u64) << 16) / (scale.width as u64);
        let step_y_fp = ((src.height as u64) << 16) / (scale.height as u64);

        for y in 0..out_dims.height {
            let v_fp = y as u64 * step_y_fp;
            let y0 = ((v_fp >> 16) as usize).min((src.height - 1) as usize);
            let y1 = (y0 + 1).min((src.height - 1) as usize);
            let fy = ((v_fp >> 8) & 0xFF) as u32;

            let row_dst_offset = (y as usize * out_dims.width as usize) * 3;
            let row0_src_offset = y0 * (src.width as usize) * 3;
            let row1_src_offset = y1 * (src.width as usize) * 3;

            for x in 0..out_dims.width {
                let u_fp = x as u64 * step_x_fp;
                let x0 = ((u_fp >> 16) as usize).min((src.width - 1) as usize);
                let x1 = (x0 + 1).min((src.width - 1) as usize);
                let fx = ((u_fp >> 8) & 0xFF) as u32;

                let p00_idx = row0_src_offset + x0 * 3;
                let p10_idx = row0_src_offset + x1 * 3;
                let p01_idx = row1_src_offset + x0 * 3;
                let p11_idx = row1_src_offset + x1 * 3;
                let dst_idx = row_dst_offset + (x as usize) * 3;

                for c in 0..3 {
                    let p00 = full[p00_idx + c] as u32;
                    let p10 = full[p10_idx + c] as u32;
                    let p01 = full[p01_idx + c] as u32;
                    let p11 = full[p11_idx + c] as u32;

                    let top = p00 * (256 - fx) + p10 * fx;
                    let bot = p01 * (256 - fx) + p11 * fx;
                    output[dst_idx + c] = ((top * (256 - fy) + bot * fy) >> 16) as u8;
                }
            }
        }

        Ok(out_dims)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MOCK_JPEG: &[u8] = include_bytes!("../../../apps/sonos/assets/album-art-mock.jpg");

    #[test]
    fn test_dimensions_and_decode() {
        let dims = dimensions(MOCK_JPEG).expect("dimensions should succeed");
        assert_eq!(dims, Dimensions { width: 640, height: 640 });

        let mut buf = alloc::vec![0u8; (dims.width * dims.height * 3) as usize];
        let decoded_dims = decode_into(MOCK_JPEG, &mut buf).expect("decode_into should succeed");
        assert_eq!(decoded_dims, dims);
        assert!(buf.iter().any(|&b| b != 0));
    }

    #[test]
    fn test_decode_scaled_and_clipped() {
        let scale = Dimensions { width: 360, height: 360 };
        let mut buf = alloc::vec![0u8; (360 * 360 * 3) as usize];
        let out = decode_scaled(MOCK_JPEG, scale, None, &mut buf).expect("decode_scaled 360x360");
        assert_eq!(out, scale);
        assert!(buf.iter().any(|&b| b != 0));

        // Test with clip
        let clip = Dimensions { width: 352, height: 352 };
        let mut clip_buf = alloc::vec![0u8; (352 * 352 * 3) as usize];
        let out_clip = decode_scaled(MOCK_JPEG, scale, Some(clip), &mut clip_buf)
            .expect("decode_scaled with clip");
        assert_eq!(out_clip, clip);
    }

    #[test]
    fn test_invalid_inputs() {
        assert_eq!(dimensions(&[]), Err(Error::InvalidData));
        assert_eq!(dimensions(&[0xFF, 0xD8, 0x00]), Err(Error::InvalidData));

        let mut tiny = [0u8; 10];
        assert_eq!(decode_into(MOCK_JPEG, &mut tiny), Err(Error::InvalidData));

        // Non-multiple of 8 scale must be rejected
        let mut buf = alloc::vec![0u8; 390 * 390 * 3];
        assert_eq!(
            decode_scaled(MOCK_JPEG, Dimensions { width: 390, height: 390 }, None, &mut buf),
            Err(Error::InvalidData)
        );
    }
}
