// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! CO5300 AMOLED controller on QSPI for LilyGO T-Encoder Pro (390x390).
//! Adapted from Slint's m5stack_stopwatch board support and t-encoder review.

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::Timer;
use esp_hal::delay::Delay;
use esp_hal::dma::DmaTxBuf;
use esp_hal::gpio::{Level, Output, OutputConfig};
use esp_hal::spi::Mode;
use esp_hal::spi::master::{Address, Command, Config as SpiConfig, DataMode, Spi, SpiDma};
use esp_hal::time::{Instant, Rate};
use esp_hal::{Async, dma_tx_buffer};

use super::board::DisplayPeripherals;

// Real display dimensions.
pub const DISPLAY_WIDTH: u16 = 390;
pub const DISPLAY_HEIGHT: u16 = 390;

// Rendering dimensions. Optimized for speed and memory usage.
pub const RENDER_WIDTH: u16 = 195;
pub const RENDER_HEIGHT: u16 = 195;

// Actual dimensions allocated by the framebuffer.
// Divisible by 16 bytes to allow aligned PIE SIMD instruction access.
pub const RENDER_STRIDE: usize = 200;
pub const BUFFER_HEIGHT: usize = 196;

pub const TX_BUF_BYTES: usize = 16 * 1024;

const QSPI_CONTROL_OPCODE: u16 = 0x02;
const QSPI_PIXEL_OPCODE: u16 = 0x32;
const CMD_RAMWR: u32 = 0x2c;
const CMD_RAMWRC: u32 = 0x3c;

pub use common::{BigEndianRgb565, NativeRgb565};

/// A borrowed framebuffer allocation from internal SRAM for baton passing.
pub struct Framebuffer(pub &'static mut [NativeRgb565; RENDER_STRIDE * BUFFER_HEIGHT]);

impl core::fmt::Debug for Framebuffer {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Framebuffer({:p})", self.0.as_ptr())
    }
}

/// A dirty rectangle in rendered half-resolution coordinates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct DirtyRect {
    pub x: u16,
    pub y: u16,
    pub width: u16,
    pub height: u16,
}

/// A display flush request containing the loaned framebuffer and dirty regions.
pub struct FlushJob {
    pub fb: Framebuffer,
    pub rects: heapless::Vec<DirtyRect, 3>,
    pub render_cycles: u32,
}

/// Commands sent to the dedicated display worker.
pub enum DisplayCommand {
    Flush(FlushJob),
    SetBrightness(u8),
    DisplayOff,
    DisplayOn,
}

pub static DISPLAY_COMMAND_CHANNEL: Channel<CriticalSectionRawMutex, DisplayCommand, 4> =
    Channel::new();
pub static FLUSH_RETURN_CHANNEL: Channel<CriticalSectionRawMutex, Framebuffer, 2> = Channel::new();

#[inline(always)]
fn cycle_count() -> u32 {
    #[cfg(target_arch = "xtensa")]
    {
        xtensa_lx::timer::get_cycle_count()
    }
    #[cfg(not(target_arch = "xtensa"))]
    {
        0
    }
}

/// The SPI peripheral and the TX DMA buffer it writes from.
pub struct Port {
    pub spi: SpiDma<'static, Async>,
    pub tx: DmaTxBuf,
}
pub struct Session<'a> {
    display: &'a mut Co5300,
    port: Option<Port>,
    first_chunk: bool,
}

impl<'a> Session<'a> {
    pub fn new(display: &'a mut Co5300, port: Port) -> Self {
        Self { display, port: Some(port), first_chunk: true }
    }

    pub fn buffer_mut(&mut self) -> &mut [u8] {
        self.port.as_mut().map(|p| p.tx.as_mut_slice()).expect("transfer in progress")
    }

    pub async fn send(&mut self, used_bytes: usize) -> Result<(), esp_hal::spi::Error> {
        let first = self.first_chunk;
        self.first_chunk = false;
        if !first {
            // Delay needed to avoid CO5300 AMOLED write pointer glitching between rects
            Delay::new().delay_micros(10);
        }
        self.send_bytes(first, used_bytes).await
    }

    async fn send_bytes(
        &mut self,
        first: bool,
        used_bytes: usize,
    ) -> Result<(), esp_hal::spi::Error> {
        let Port { spi, tx } = self.port.take().expect("DMA in progress");
        let address: u32 = if first { CMD_RAMWR } else { CMD_RAMWRC } << 8;
        let mut transfer = spi
            .half_duplex_write(
                DataMode::Quad,
                Command::_8Bit(QSPI_PIXEL_OPCODE, DataMode::Single),
                Address::_24Bit(address, DataMode::Single),
                0,
                used_bytes,
                tx,
            )
            .map_err(|(e, spi, tx)| {
                self.port = Some(Port { spi, tx });
                e
            })?;
        transfer.wait_for_done().await;
        // Sync `.wait()` shouldn't block after transfer is done.
        let (spi, tx) = transfer.wait();
        self.port = Some(Port { spi, tx });
        Ok(())
    }
}

impl<'a> Drop for Session<'a> {
    fn drop(&mut self) {
        self.display.port =
            Some(self.port.take().expect("missing port. did the last transfer finish?"));
    }
}

pub struct Co5300 {
    port: Option<Port>,
    pub power_en: Output<'static>,
    pub reset_pin: Output<'static>,
}

unsafe impl Send for Co5300 {}

impl Co5300 {
    pub fn new(p: DisplayPeripherals) -> Self {
        let spi = Spi::new(
            p.spi,
            SpiConfig::default().with_frequency(Rate::from_mhz(80)).with_mode(Mode::_0),
        )
        .unwrap()
        .with_sio0(p.sio0)
        .with_sio1(p.sio1)
        .with_sio2(p.sio2)
        .with_sio3(p.sio3)
        .with_cs(p.cs)
        .with_sck(p.sck)
        .with_dma(p.dma_channel)
        .into_async();

        let tx = dma_tx_buffer!(TX_BUF_BYTES).unwrap();
        let power_en = Output::new(p.power_en, Level::Low, OutputConfig::default());
        let reset_pin = Output::new(p.reset_pin, Level::High, OutputConfig::default());
        Self { port: Some(Port { spi, tx }), power_en, reset_pin }
    }

    pub async fn start_session(
        &mut self,
        x: u16,
        y: u16,
        width: u16,
        height: u16,
    ) -> Result<Session<'_>, esp_hal::spi::Error> {
        // Send window commands while Co5300 still owns the Port
        self.set_window(x, y, width, height).await?;

        Ok(self.port.take().map(|p| Session::new(self, p)).unwrap())
    }

    pub async fn power_on_and_reset(&mut self) {
        // Enable panel power rail
        self.power_en.set_high();
        Timer::after_millis(50).await;

        // Hardware reset sequence
        self.reset_pin.set_low();
        Timer::after_millis(20).await;
        self.reset_pin.set_high();
        Timer::after_millis(150).await;
    }

    pub async fn init(&mut self) -> Result<(), esp_hal::spi::Error> {
        self.power_on_and_reset().await;

        self.command(0x11, &[]).await?; // sleep out
        Timer::after_millis(120).await;
        self.command(0x34, &[0x00]).await?; // tearing effect off
        self.command(0xfe, &[0x00]).await?; // switch to the user command page
        self.command(0xc4, &[0x80]).await?; // QSPI mode
        self.command(0x3a, &[0x55]).await?; // 16 bits per pixel (RGB565)
        self.command(0x36, &[0x00]).await?; // memory access control
        self.command(0x53, &[0x20]).await?; // brightness control on
        self.command(0x63, &[0xff]).await?; // brightness in high brightness mode
        self.command(0x29, &[]).await?; // display on
        self.command(0x51, &[0xff]).await?; // brightness in normal mode
        self.command(0x58, &[0x00]).await?; // high contrast mode off

        Ok(())
    }

    /// Sets display brightness level (0..255).
    pub async fn set_brightness(&mut self, level: u8) -> Result<(), esp_hal::spi::Error> {
        self.command(0x51, &[level]).await
    }

    /// Turns the display on.
    pub async fn display_on(&mut self) -> Result<(), esp_hal::spi::Error> {
        self.command(0x29, &[]).await
    }

    /// Turns the display off.
    pub async fn display_off(&mut self) -> Result<(), esp_hal::spi::Error> {
        self.command(0x28, &[]).await
    }

    async fn command(&mut self, command: u8, parameters: &[u8]) -> Result<(), esp_hal::spi::Error> {
        let Port { spi, mut tx } = self.port.take().unwrap();
        let len = parameters.len();
        if len > 0 {
            tx.as_mut_slice()[..len].copy_from_slice(parameters);
        }
        let mut transfer = spi
            .half_duplex_write(
                DataMode::Single,
                Command::_8Bit(QSPI_CONTROL_OPCODE, DataMode::Single),
                Address::_24Bit((command as u32) << 8, DataMode::Single),
                0,
                len,
                tx,
            )
            .map_err(|(e, spi, tx)| {
                self.port = Some(Port { spi, tx });
                e
            })?;
        transfer.wait_for_done().await;
        // Sync `.wait()` shouldn't block after transfer is done.
        let (spi, tx) = transfer.wait();
        self.port = Some(Port { spi, tx });
        Ok(())
    }

    async fn set_window(
        &mut self,
        x: u16,
        y: u16,
        width: u16,
        height: u16,
    ) -> Result<(), esp_hal::spi::Error> {
        const CASET: u8 = 0x2A;
        const RASET: u8 = 0x2B;

        let (x0, x1) = (x, x + width - 1);
        let (y0, y1) = (y, y + height - 1);
        self.command(CASET, &[(x0 >> 8) as u8, x0 as u8, (x1 >> 8) as u8, x1 as u8])
            .await?;
        self.command(RASET, &[(y0 >> 8) as u8, y0 as u8, (y1 >> 8) as u8, y1 as u8])
            .await
    }
}

/// Expands `logical_rows` from `frame_buffer` into `tx_slice` with Scale2x / EPX upscaling.
///
/// Scale2x / EPX inspects the 4 cardinal neighbors (U, D, L, R) of each pixel P:
/// - If U != D and L != R:
///   - Top-left:     E0 = (L == U) ? U : P
///   - Top-right:    E1 = (R == U) ? U : P
///   - Bottom-left:  E2 = (L == D) ? D : P
///   - Bottom-right: E3 = (R == D) ? D : P
/// - Else: all 4 subpixels remain P.
///
/// Because it avoids color averaging, straight horizontal/vertical font strokes remain
/// 100% sharp and crisp (no ink-blot smearing), while circular curves and diagonals
/// are smoothed cleanly.
///
/// Returns the total number of bytes written to `tx_slice`.
#[esp_hal::ram]
pub fn expand_2x2_chunk(
    frame_buffer: &[NativeRgb565],
    stride: usize,
    x: usize,
    y: usize,
    width: usize,
    logical_rows: usize,
    tx_slice: &mut [u8],
) -> usize {
    let phys_row_words = width; // 1 word = 2 pixels = 4 bytes
    let two_rows_bytes = phys_row_words * 8; // 2 physical rows: width * 4 * 2 bytes

    assert!(tx_slice.len() >= logical_rows * two_rows_bytes);
    assert!((tx_slice.as_ptr() as usize & 0x3) == 0);

    let tx_words: &mut [u32] = unsafe {
        core::slice::from_raw_parts_mut(
            tx_slice.as_mut_ptr() as *mut u32,
            logical_rows * phys_row_words * 2,
        )
    };

    let max_col = (stride - 1).min(RENDER_WIDTH as usize - 1);
    let total_rows = frame_buffer.len() / stride;
    let max_row = (total_rows - 1).min(RENDER_HEIGHT as usize - 1);

    for l in 0..logical_rows {
        let curr_y = y + l;
        let prev_y = curr_y.saturating_sub(1);
        let next_y = (curr_y + 1).min(max_row);

        let prev_row = &frame_buffer[prev_y * stride..];
        let curr_row = &frame_buffer[curr_y * stride..];
        let next_row = &frame_buffer[next_y * stride..];

        let row0_start = l * phys_row_words * 2;
        let row1_start = row0_start + phys_row_words;

        let mut c = 0;
        let safe_unroll_limit = if width >= 4 && (x + c) > 0 && x + 4 <= max_col {
            (max_col - 4 - x).min(width - 4)
        } else {
            usize::MAX
        };

        if safe_unroll_limit != usize::MAX {
            while c <= safe_unroll_limit {
                let col = x + c;

                let p0 = curr_row[col].raw();
                let p1 = curr_row[col + 1].raw();
                let p2 = curr_row[col + 2].raw();
                let p3 = curr_row[col + 3].raw();

                let u0 = prev_row[col].raw();
                let u1 = prev_row[col + 1].raw();
                let u2 = prev_row[col + 2].raw();
                let u3 = prev_row[col + 3].raw();

                let d0 = next_row[col].raw();
                let d1 = next_row[col + 1].raw();
                let d2 = next_row[col + 2].raw();
                let d3 = next_row[col + 3].raw();

                let l0 = curr_row[col - 1].raw();
                let r3 = curr_row[col + 4].raw();

                // Col 0
                let (e0_0, e1_0, e2_0, e3_0) = if u0 != d0 && l0 != p1 {
                    (
                        if l0 == u0 { u0 } else { p0 },
                        if p1 == u0 { u0 } else { p0 },
                        if l0 == d0 { d0 } else { p0 },
                        if p1 == d0 { d0 } else { p0 },
                    )
                } else {
                    (p0, p0, p0, p0)
                };

                // Col 1
                let (e0_1, e1_1, e2_1, e3_1) = if u1 != d1 && p0 != p2 {
                    (
                        if p0 == u1 { u1 } else { p1 },
                        if p2 == u1 { u1 } else { p1 },
                        if p0 == d1 { d1 } else { p1 },
                        if p2 == d1 { d1 } else { p1 },
                    )
                } else {
                    (p1, p1, p1, p1)
                };

                // Col 2
                let (e0_2, e1_2, e2_2, e3_2) = if u2 != d2 && p1 != p3 {
                    (
                        if p1 == u2 { u2 } else { p2 },
                        if p3 == u2 { u2 } else { p2 },
                        if p1 == d2 { d2 } else { p2 },
                        if p3 == d2 { d2 } else { p2 },
                    )
                } else {
                    (p2, p2, p2, p2)
                };

                // Col 3
                let (e0_3, e1_3, e2_3, e3_3) = if u3 != d3 && p2 != r3 {
                    (
                        if p2 == u3 { u3 } else { p3 },
                        if r3 == u3 { u3 } else { p3 },
                        if p2 == d3 { d3 } else { p3 },
                        if r3 == d3 { d3 } else { p3 },
                    )
                } else {
                    (p3, p3, p3, p3)
                };

                tx_words[row0_start + c] = (e0_0.to_be() as u32) | ((e1_0.to_be() as u32) << 16);
                tx_words[row0_start + c + 1] =
                    (e0_1.to_be() as u32) | ((e1_1.to_be() as u32) << 16);
                tx_words[row0_start + c + 2] =
                    (e0_2.to_be() as u32) | ((e1_2.to_be() as u32) << 16);
                tx_words[row0_start + c + 3] =
                    (e0_3.to_be() as u32) | ((e1_3.to_be() as u32) << 16);

                tx_words[row1_start + c] = (e2_0.to_be() as u32) | ((e3_0.to_be() as u32) << 16);
                tx_words[row1_start + c + 1] =
                    (e2_1.to_be() as u32) | ((e3_1.to_be() as u32) << 16);
                tx_words[row1_start + c + 2] =
                    (e2_2.to_be() as u32) | ((e3_2.to_be() as u32) << 16);
                tx_words[row1_start + c + 3] =
                    (e2_3.to_be() as u32) | ((e3_3.to_be() as u32) << 16);

                c += 4;
            }
        }

        // Tail loop with edge clamping
        while c < width {
            let col = x + c;
            let left_col = col.saturating_sub(1);
            let right_col = (col + 1).min(max_col);

            let p = curr_row[col].raw();
            let u = prev_row[col].raw();
            let d = next_row[col].raw();
            let l = curr_row[left_col].raw();
            let r = curr_row[right_col].raw();

            let (e0, e1, e2, e3) = if u != d && l != r {
                (
                    if l == u { u } else { p },
                    if r == u { u } else { p },
                    if l == d { d } else { p },
                    if r == d { d } else { p },
                )
            } else {
                (p, p, p, p)
            };

            tx_words[row0_start + c] = (e0.to_be() as u32) | ((e1.to_be() as u32) << 16);
            tx_words[row1_start + c] = (e2.to_be() as u32) | ((e3.to_be() as u32) << 16);

            c += 1;
        }
    }

    logical_rows * two_rows_bytes
}

/// Background worker task that exclusively owns the CO5300 display and processes flush jobs.
#[embassy_executor::task]
pub async fn display_task(mut display: Co5300) {
    if let Err(e) = display.init().await {
        defmt::error!("Failed to initialize CO5300 display: {}", e);
    } else {
        defmt::info!("CO5300 display initialized successfully");
    }

    let mut perf_tracker = app_shell::PerfTracker::new();
    let loop_start_time = Instant::now();

    loop {
        match DISPLAY_COMMAND_CHANNEL.receive().await {
            DisplayCommand::Flush(job) => {
                let t_start = cycle_count();
                for rect in &job.rects {
                    let phys_x = rect.x * 2;
                    let phys_y = rect.y * 2;
                    let phys_width = rect.width * 2;
                    let phys_height = rect.height * 2;

                    let mut session = match display
                        .start_session(phys_x, phys_y, phys_width, phys_height)
                        .await
                    {
                        Ok(s) => s,
                        Err(e) => {
                            defmt::error!("Failed to start session: {:?}", defmt::Debug2Format(&e));
                            break;
                        }
                    };

                    let logical_row_bytes = rect.width as usize * 8;
                    let logical_rows_per_chunk = (TX_BUF_BYTES / logical_row_bytes).max(1);
                    let mut row = rect.y;
                    while row < rect.y + rect.height {
                        let logical_rows =
                            logical_rows_per_chunk.min((rect.y + rect.height - row) as usize);
                        let used = expand_2x2_chunk(
                            &job.fb.0[..],
                            RENDER_STRIDE,
                            rect.x as usize,
                            row as usize,
                            rect.width as usize,
                            logical_rows,
                            session.buffer_mut(),
                        );
                        if let Err(e) = session.send(used).await {
                            defmt::error!("Session send error: {:?}", defmt::Debug2Format(&e));
                            break;
                        }
                        row += logical_rows as u16;
                    }
                }
                let transfer_cycles = cycle_count().wrapping_sub(t_start);
                let _ = FLUSH_RETURN_CHANNEL.send(job.fb).await;

                if transfer_cycles > 0 {
                    let dirty_pixels: u32 = job
                        .rects
                        .iter()
                        .map(|r| (r.width as u32 * 2) * (r.height as u32 * 2))
                        .sum();
                    let rect_count = job.rects.len() as u16;
                    perf_tracker.record_frame(app_shell::FrameCycles {
                        render_cycles: job.render_cycles,
                        transfer_cycles,
                        dirty_pixels,
                        rect_count,
                    });
                }

                let now_since_start = core::time::Duration::from_micros(
                    (Instant::now() - loop_start_time).as_micros(),
                );
                if let Some(summary) = perf_tracker.take_summary(now_since_start) {
                    defmt::debug!("[PERF] {}", summary);
                }
            }
            DisplayCommand::SetBrightness(level) => {
                let _ = display.set_brightness(level).await;
            }
            DisplayCommand::DisplayOff => {
                let _ = display.display_off().await;
            }
            DisplayCommand::DisplayOn => {
                let _ = display.display_on().await;
            }
        }
    }
}
