//! Rust wrapper around [JPEGDEC](https://github.com/bitbank2/JPEGDEC) for
//! baseline JPEG decoding into RGB8 buffers.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use core::ffi::{c_int, c_uchar, c_void};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidData,
    UnsupportedFeature,
    DecodeError,
}

/// Decoded image dimensions.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dimensions {
    pub width: u32,
    pub height: u32,
}

/// Memory for the decoder (~18 KB), from the heap rather than the caller's
/// stack: a task's stack may be smaller than that.
fn workspace() -> Vec<u64> {
    let size = unsafe { jpegdec_workspace_size() };
    alloc::vec![0u64; size.div_ceil(core::mem::size_of::<u64>())]
}

/// Returns the dimensions of the JPEG image without decoding the pixel data.
pub fn dimensions(jpeg_data: &[u8]) -> Result<Dimensions, Error> {
    let mut workspace = workspace();
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    let ok = unsafe {
        jpegdec_get_dimensions(
            workspace.as_mut_ptr().cast::<c_void>(),
            jpeg_data.as_ptr(),
            jpeg_data.len() as c_int,
            &mut width,
            &mut height,
        )
    };
    if ok != 1 {
        return Err(Error::InvalidData);
    }
    Ok(Dimensions { width: width as u32, height: height as u32 })
}

/// Decodes a JPEG image into an RGB8 (`[R, G, B]`) buffer.
///
/// `output` must be at least `width * height * 3` bytes.
pub fn decode_into(jpeg_data: &[u8], output: &mut [u8]) -> Result<Dimensions, Error> {
    let dims = dimensions(jpeg_data)?;
    let expected = (dims.width as usize) * (dims.height as usize) * 3;
    if output.len() < expected {
        return Err(Error::InvalidData);
    }

    let mut workspace = workspace();
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    let ok = unsafe {
        jpegdec_decode_rgb8(
            workspace.as_mut_ptr().cast::<c_void>(),
            jpeg_data.as_ptr(),
            jpeg_data.len() as c_int,
            output.as_mut_ptr(),
            &mut width,
            &mut height,
        )
    };
    if ok != 1 {
        return Err(Error::DecodeError);
    }

    Ok(Dimensions { width: width as u32, height: height as u32 })
}

unsafe extern "C" {
    fn jpegdec_workspace_size() -> usize;

    fn jpegdec_get_dimensions(
        workspace: *mut c_void,
        data: *const c_uchar,
        size: c_int,
        width: *mut c_int,
        height: *mut c_int,
    ) -> c_int;

    fn jpegdec_decode_rgb8(
        workspace: *mut c_void,
        data: *const c_uchar,
        size: c_int,
        output: *mut c_uchar,
        width: *mut c_int,
        height: *mut c_int,
    ) -> c_int;
}
