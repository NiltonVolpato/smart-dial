// Stub for the ESP-DSP header expected by JPEGDEC's ESP32-S3 SIMD assembly.
// The actual ESP-DSP library is not needed; we only need to signal that the
// S3's PIE SIMD extensions are available.
#pragma once
#define dsps_fft2r_sc16_aes3_enabled 1
