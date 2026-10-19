// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Benchmark for album-art JPEG decoding, scaling, clipping, and dimming on ESP32-S3.
//!
//! Run on device with:
//!   just device::exec cargo test -p lilygo_t_encoder_pro --test album_art_benchmark --release

#![no_std]
#![no_main]

extern crate alloc;

esp_bootloader_esp_idf::esp_app_desc!();

#[cfg(test)]
#[embedded_test::tests(executor = esp_rtos::embassy::Executor::new())]
mod tests {
    use defmt::info;
    use esp_hal::clock::CpuClock;
    use lilygo_t_encoder_pro::bsp::simd;
    use xtensa_lx::timer::get_cycle_count;

    const MOCK_ALBUM_ART_JPEG: &[u8] =
        include_bytes!("../../../portable/apps/sonos/assets/album-art-mock.jpg");
    const RUNS: usize = 5;

    #[init]
    fn init() {
        let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
        let peripherals = esp_hal::init(config);

        esp_alloc::psram_allocator!(peripherals.PSRAM, esp_hal::psram);
        esp_alloc::heap_allocator!(#[esp_hal::ram(reclaimed)] size: 73744);
        esp_alloc::heap_allocator!(size: 64 * 1024);

        let timg0 = esp_hal::timer::timg::TimerGroup::new(peripherals.TIMG0);
        let sw_interrupt =
            esp_hal::interrupt::software::SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
        esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

        rtt_target::rtt_init_defmt!();
    }

    #[test]
    async fn benchmark_album_art() {
        simd::enable_pie();

        info!("=== Album Art JPEG Benchmark ({} runs) ===", RUNS);
        info!("Input JPEG size: {} bytes", MOCK_ALBUM_ART_JPEG.len());

        // 1. Header parsing (dimensions)
        let dims = jpeg::dimensions(MOCK_ALBUM_ART_JPEG).expect("Failed to read dimensions");
        let dims_cycles = bench_best(|| {
            let d = jpeg::dimensions(MOCK_ALBUM_ART_JPEG).unwrap();
            core::hint::black_box(d);
        });
        info!(
            "1. dimensions ({}x{}): {} cycles ({} us)",
            dims.width,
            dims.height,
            dims_cycles,
            dims_cycles / 240
        );

        // 2. Raw full-res RGB888 decode (640x640)
        let mut full_rgb =
            slint::SharedPixelBuffer::<slint::Rgb8Pixel>::new(dims.width, dims.height);
        let full_bytes = full_rgb.make_mut_bytes();
        let raw_decode_cycles = bench_best(|| {
            let d = jpeg::decode_into(MOCK_ALBUM_ART_JPEG, full_bytes).unwrap();
            core::hint::black_box(d);
        });
        info!(
            "2. raw decode_into ({}x{} RGB888): {} cycles ({} us / {} ms)",
            dims.width,
            dims.height,
            raw_decode_cycles,
            raw_decode_cycles / 240,
            raw_decode_cycles / 240_000
        );
        drop(full_rgb);

        // 3. Scaled RGB888 decode (640x640 -> 360x360) via esp_new_jpeg
        let mut scaled_rgb = slint::SharedPixelBuffer::<slint::Rgb8Pixel>::new(360, 360);
        let scaled_bytes = scaled_rgb.make_mut_bytes();
        let scaled_decode_cycles = bench_best(|| {
            let d = jpeg::decode_scaled(
                MOCK_ALBUM_ART_JPEG,
                jpeg::Dimensions { width: 360, height: 360 },
                None,
                scaled_bytes,
            )
            .unwrap();
            core::hint::black_box(d);
        });
        info!(
            "3. decode_scaled (360x360 RGB888): {} cycles ({} us / {} ms)",
            scaled_decode_cycles,
            scaled_decode_cycles / 240,
            scaled_decode_cycles / 240_000
        );

        // 4. In-place dimming pass (360x360 RGB888)
        let dim_cycles = bench_best(|| {
            album_art::dim_rgb8_in_place(scaled_bytes, 65);
            core::hint::black_box(&scaled_bytes);
        });
        info!(
            "4. dim_rgb8_in_place (360x360): {} cycles ({} us / {} ms)",
            dim_cycles,
            dim_cycles / 240,
            dim_cycles / 240_000
        );
        drop(scaled_rgb);

        // 5. End-to-end album_art::decode for 360x360
        let heap_before = esp_alloc::HEAP.stats();
        let e2e_360_cycles = bench_best(|| {
            let pixels = album_art::decode(MOCK_ALBUM_ART_JPEG, 360).unwrap();
            defmt::assert_eq!(pixels.width(), 360);
            defmt::assert_eq!(pixels.height(), 360);
            core::hint::black_box(pixels);
        });
        let heap_after_360 = esp_alloc::HEAP.stats();
        info!(
            "5. album_art::decode (360x360): {} cycles ({} us / {} ms)",
            e2e_360_cycles,
            e2e_360_cycles / 240,
            e2e_360_cycles / 240_000
        );

        // 6. End-to-end album_art::decode for 390x390 (rounded to 392x392)
        let e2e_390_cycles = bench_best(|| {
            let pixels = album_art::decode(MOCK_ALBUM_ART_JPEG, 390).unwrap();
            defmt::assert_eq!(pixels.width(), 392);
            defmt::assert_eq!(pixels.height(), 392);
            core::hint::black_box(pixels);
        });
        info!(
            "6. album_art::decode (390 -> 392x392): {} cycles ({} us / {} ms)",
            e2e_390_cycles,
            e2e_390_cycles / 240,
            e2e_390_cycles / 240_000
        );
        info!("Heap stats before: {}", heap_before);
        info!("Heap stats after:  {}", heap_after_360);
    }

    fn bench_best<F: FnMut()>(mut f: F) -> u32 {
        let mut best = u32::MAX;
        for _ in 0..RUNS {
            let start = get_cycle_count();
            f();
            let end = get_cycle_count();
            let cycles = end.wrapping_sub(start);
            if cycles < best {
                best = cycles;
            }
        }
        best
    }
}
