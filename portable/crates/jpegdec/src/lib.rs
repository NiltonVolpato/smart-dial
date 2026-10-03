//! Rust wrapper around [JPEGDEC](https://github.com/bitbank2/JPEGDEC) for
//! baseline JPEG decoding into RGB8 buffers.

#![no_std]

use core::ffi::{c_int, c_uchar};

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

/// Returns the dimensions of the JPEG image without decoding the pixel data.
pub fn dimensions(jpeg_data: &[u8]) -> Result<Dimensions, Error> {
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    let ok = unsafe {
        jpegdec_get_dimensions(
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

    let mut width: c_int = 0;
    let mut height: c_int = 0;
    let ok = unsafe {
        jpegdec_decode_rgb8(
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
    fn jpegdec_get_dimensions(
        data: *const c_uchar,
        size: c_int,
        width: *mut c_int,
        height: *mut c_int,
    ) -> c_int;

    fn jpegdec_decode_rgb8(
        data: *const c_uchar,
        size: c_int,
        output: *mut c_uchar,
        width: *mut c_int,
        height: *mut c_int,
    ) -> c_int;
}
