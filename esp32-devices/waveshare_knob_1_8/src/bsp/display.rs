// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! SH8601 controller on QSPI with DMA for Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (360x360).

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::Timer;
use esp_hal::dma::{DmaDescriptor, DmaTxBuf};
use esp_hal::gpio::{DriveMode, Level, Output, OutputConfig};
use esp_hal::ledc::channel::{self, ChannelIFace};
use esp_hal::ledc::timer::{self, TimerIFace};
use esp_hal::ledc::{LSGlobalClkSource, Ledc, LowSpeed};
use esp_hal::spi::Mode;
use esp_hal::spi::master::{Address, Command, Config as SpiConfig, DataMode, Spi, SpiDma};
use esp_hal::time::{Instant, Rate};
use esp_hal::{Async, dma_tx_buffer};

use super::board::DisplayPeripherals;

// Native display dimensions.
pub const DISPLAY_WIDTH: u16 = 360;
pub const DISPLAY_HEIGHT: u16 = 360;

// Rendering dimensions (1:1 native resolution).
pub const RENDER_WIDTH: u16 = 360;
pub const RENDER_HEIGHT: u16 = 360;
pub const RENDER_STRIDE: usize = 360;

const COMMAND_BUFFER_BYTES: usize = 64;
const FRAMEBUFFER_DMA_DESCRIPTORS_COUNT: usize = 8;
const MAX_CHUNK_ROWS: usize = 40;

const QSPI_CONTROL_OPCODE: u16 = 0x02;
const QSPI_PIXEL_OPCODE: u16 = 0x32;
const CMD_RAMWR: u32 = 0x2C;
const CMD_RAMWRC: u32 = 0x3C;
const CMD_CASET: u8 = 0x2A;
const CMD_RASET: u8 = 0x2B;

pub use common::{BigEndianRgb565, NativeRgb565};

#[derive(Clone, Copy, Debug)]
pub struct DirtyRect {
    pub x: u16,
    pub y: u16,
    pub width: u16,
    pub height: u16,
}

pub struct Framebuffer(pub &'static mut [BigEndianRgb565]);
unsafe impl Send for Framebuffer {}

impl core::fmt::Debug for Framebuffer {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Framebuffer({:p})", self.0.as_ptr())
    }
}

pub struct FlushJob {
    pub fb: Framebuffer,
    pub rects: heapless::Vec<DirtyRect, 4>,
    pub render_cycles: u32,
}

pub enum DisplayCommand {
    Flush(FlushJob),
    SetBrightness(u8),
    DisplayOff,
    DisplayOn,
}

pub static DISPLAY_COMMAND_CHANNEL: Channel<CriticalSectionRawMutex, DisplayCommand, 2> =
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

/// The SPI peripheral, command DMA buffer, and framebuffer DMA descriptors.
pub struct Port {
    pub spi: SpiDma<'static, Async>,
    pub command_tx_buffer: DmaTxBuf,
    pub framebuffer_descriptors: &'static mut [DmaDescriptor],
}

pub struct Session<'a> {
    display: &'a mut Sh8601,
    port: Option<Port>,
    first_chunk: bool,
}

impl<'a> Session<'a> {
    pub fn new(display: &'a mut Sh8601, port: Port) -> Self {
        Self { display, port: Some(port), first_chunk: true }
    }

    pub async fn send_pixel_chunk(
        &mut self,
        pixel_bytes: &'static mut [u8],
    ) -> Result<(), esp_hal::spi::Error> {
        let first = self.first_chunk;
        self.first_chunk = false;
        let Port { spi, command_tx_buffer, framebuffer_descriptors } =
            self.port.take().expect("DMA in progress");

        let address: u32 = if first { CMD_RAMWR } else { CMD_RAMWRC } << 8;
        let transfer_length = pixel_bytes.len();
        let tx_buffer = DmaTxBuf::new(framebuffer_descriptors, pixel_bytes)
            .expect("Failed to initialize Framebuffer DMA buffer");

        let mut transfer = match spi.half_duplex_write(
            DataMode::Quad,
            Command::_8Bit(QSPI_PIXEL_OPCODE, DataMode::Single),
            Address::_24Bit(address, DataMode::Single),
            0,
            transfer_length,
            tx_buffer,
        ) {
            Ok(transfer) => transfer,
            Err((e, spi, tx_buffer)) => {
                let (framebuffer_descriptors, _) = tx_buffer.split();
                self.port = Some(Port { spi, command_tx_buffer, framebuffer_descriptors });
                return Err(e);
            }
        };
        transfer.wait_for_done().await;
        let (spi, tx_buffer) = transfer.wait();
        let (framebuffer_descriptors, _) = tx_buffer.split();
        self.port = Some(Port { spi, command_tx_buffer, framebuffer_descriptors });
        Ok(())
    }
}

impl<'a> Drop for Session<'a> {
    fn drop(&mut self) {
        self.display.port =
            Some(self.port.take().expect("missing port. did the last transfer finish?"));
    }
}

pub struct Sh8601 {
    port: Option<Port>,
    pub reset_pin: Output<'static>,
    pub bl_channel: channel::Channel<'static, LowSpeed>,
}

unsafe impl Send for Sh8601 {}

static BACKLIGHT_TIMER: static_cell::StaticCell<timer::Timer<'static, LowSpeed>> =
    static_cell::StaticCell::new();
static FRAMEBUFFER_DMA_DESCRIPTORS: static_cell::StaticCell<
    [DmaDescriptor; FRAMEBUFFER_DMA_DESCRIPTORS_COUNT],
> = static_cell::StaticCell::new();

impl Sh8601 {
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

        let command_tx_buffer = dma_tx_buffer!(COMMAND_BUFFER_BYTES).unwrap();
        let framebuffer_descriptors = FRAMEBUFFER_DMA_DESCRIPTORS
            .init([DmaDescriptor::EMPTY; FRAMEBUFFER_DMA_DESCRIPTORS_COUNT]);
        let reset_pin = Output::new(p.reset_pin, Level::High, OutputConfig::default());

        let mut ledc = Ledc::new(p.ledc);
        ledc.set_global_slow_clock(LSGlobalClkSource::APBClk);
        let raw_timer = ledc.timer::<LowSpeed>(timer::Number::Timer3);
        let lstimer = BACKLIGHT_TIMER.init(raw_timer);
        lstimer
            .configure(timer::config::Config {
                duty: timer::config::Duty::Duty8Bit,
                clock_source: timer::LSClockSource::APBClk,
                frequency: Rate::from_khz(5),
            })
            .unwrap();

        let mut bl_channel = ledc.channel(channel::Number::Channel1, p.backlight);
        bl_channel
            .configure(channel::config::Config {
                timer: lstimer,
                duty_pct: 100,
                drive_mode: DriveMode::PushPull,
            })
            .unwrap();

        Self {
            port: Some(Port { spi, command_tx_buffer, framebuffer_descriptors }),
            reset_pin,
            bl_channel,
        }
    }

    pub async fn power_on_and_reset(&mut self) {
        self.reset_pin.set_low();
        Timer::after_millis(20).await;
        self.reset_pin.set_high();
        Timer::after_millis(150).await;
    }

    pub async fn start_session(
        &mut self,
        x: u16,
        y: u16,
        width: u16,
        height: u16,
    ) -> Result<Session<'_>, esp_hal::spi::Error> {
        self.set_window(x, y, width, height).await?;
        Ok(self.port.take().map(|p| Session::new(self, p)).unwrap())
    }

    async fn set_window(
        &mut self,
        x: u16,
        y: u16,
        width: u16,
        height: u16,
    ) -> Result<(), esp_hal::spi::Error> {
        let (x0, x1) = (x, (x + width - 1).min(DISPLAY_WIDTH - 1));
        let (y0, y1) = (y, (y + height - 1).min(DISPLAY_HEIGHT - 1));
        self.command(CMD_CASET, &[(x0 >> 8) as u8, x0 as u8, (x1 >> 8) as u8, x1 as u8])
            .await?;
        self.command(CMD_RASET, &[(y0 >> 8) as u8, y0 as u8, (y1 >> 8) as u8, y1 as u8])
            .await
    }

    #[allow(clippy::too_many_lines)]
    pub async fn init(&mut self) -> Result<(), esp_hal::spi::Error> {
        self.power_on_and_reset().await;

        // Sequence from Waveshare 08_LVGL_Test reference driver
        self.command(0xF0, &[0x28]).await?;
        self.command(0xF2, &[0x28]).await?;
        self.command(0x73, &[0xF0]).await?;
        self.command(0x7C, &[0xD1]).await?;
        self.command(0x83, &[0xE0]).await?;
        self.command(0x84, &[0x61]).await?;
        self.command(0xF2, &[0x82]).await?;
        self.command(0xF0, &[0x00]).await?;
        self.command(0xF0, &[0x01]).await?;
        self.command(0xF1, &[0x01]).await?;
        self.command(0xB0, &[0x56]).await?;
        self.command(0xB1, &[0x4D]).await?;
        self.command(0xB2, &[0x24]).await?;
        self.command(0xB4, &[0x87]).await?;
        self.command(0xB5, &[0x44]).await?;
        self.command(0xB6, &[0x8B]).await?;
        self.command(0xB7, &[0x40]).await?;
        self.command(0xB8, &[0x86]).await?;
        self.command(0xBA, &[0x00]).await?;
        self.command(0xBB, &[0x08]).await?;
        self.command(0xBC, &[0x08]).await?;
        self.command(0xBD, &[0x00]).await?;
        self.command(0xC0, &[0x80]).await?;
        self.command(0xC1, &[0x10]).await?;
        self.command(0xC2, &[0x37]).await?;
        self.command(0xC3, &[0x80]).await?;
        self.command(0xC4, &[0x10]).await?;
        self.command(0xC5, &[0x37]).await?;
        self.command(0xC6, &[0xA9]).await?;
        self.command(0xC7, &[0x41]).await?;
        self.command(0xC8, &[0x01]).await?;
        self.command(0xC9, &[0xA9]).await?;
        self.command(0xCA, &[0x41]).await?;
        self.command(0xCB, &[0x01]).await?;
        self.command(0xD0, &[0x91]).await?;
        self.command(0xD1, &[0x68]).await?;
        self.command(0xD2, &[0x68]).await?;
        self.command(0xF5, &[0x00, 0xA5]).await?;
        self.command(0xDD, &[0x4F]).await?;
        self.command(0xDE, &[0x4F]).await?;
        self.command(0xF1, &[0x10]).await?;
        self.command(0xF0, &[0x00]).await?;
        self.command(0xF0, &[0x02]).await?;
        self.command(
            0xE0,
            &[
                0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34,
            ],
        )
        .await?;
        self.command(
            0xE1,
            &[
                0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33,
            ],
        )
        .await?;
        self.command(0xF0, &[0x10]).await?;
        self.command(0xF3, &[0x10]).await?;
        self.command(0xE0, &[0x07]).await?;
        self.command(0xE1, &[0x00]).await?;
        self.command(0xE2, &[0x00]).await?;
        self.command(0xE3, &[0x00]).await?;
        self.command(0xE4, &[0xE0]).await?;
        self.command(0xE5, &[0x06]).await?;
        self.command(0xE6, &[0x21]).await?;
        self.command(0xE7, &[0x01]).await?;
        self.command(0xE8, &[0x05]).await?;
        self.command(0xE9, &[0x02]).await?;
        self.command(0xEA, &[0xDA]).await?;
        self.command(0xEB, &[0x00]).await?;
        self.command(0xEC, &[0x00]).await?;
        self.command(0xED, &[0x0F]).await?;
        self.command(0xEE, &[0x00]).await?;
        self.command(0xEF, &[0x00]).await?;
        self.command(0xF8, &[0x00]).await?;
        self.command(0xF9, &[0x00]).await?;
        self.command(0xFA, &[0x00]).await?;
        self.command(0xFB, &[0x00]).await?;
        self.command(0xFC, &[0x00]).await?;
        self.command(0xFD, &[0x00]).await?;
        self.command(0xFE, &[0x00]).await?;
        self.command(0xFF, &[0x00]).await?;
        self.command(0x60, &[0x40]).await?;
        self.command(0x61, &[0x04]).await?;
        self.command(0x62, &[0x00]).await?;
        self.command(0x63, &[0x42]).await?;
        self.command(0x64, &[0xD9]).await?;
        self.command(0x65, &[0x00]).await?;
        self.command(0x66, &[0x00]).await?;
        self.command(0x67, &[0x00]).await?;
        self.command(0x68, &[0x00]).await?;
        self.command(0x69, &[0x00]).await?;
        self.command(0x6A, &[0x00]).await?;
        self.command(0x6B, &[0x00]).await?;
        self.command(0x70, &[0x40]).await?;
        self.command(0x71, &[0x03]).await?;
        self.command(0x72, &[0x00]).await?;
        self.command(0x73, &[0x42]).await?;
        self.command(0x74, &[0xD8]).await?;
        self.command(0x75, &[0x00]).await?;
        self.command(0x76, &[0x00]).await?;
        self.command(0x77, &[0x00]).await?;
        self.command(0x78, &[0x00]).await?;
        self.command(0x79, &[0x00]).await?;
        self.command(0x7A, &[0x00]).await?;
        self.command(0x7B, &[0x00]).await?;
        self.command(0x80, &[0x48]).await?;
        self.command(0x81, &[0x00]).await?;
        self.command(0x82, &[0x06]).await?;
        self.command(0x83, &[0x02]).await?;
        self.command(0x84, &[0xD6]).await?;
        self.command(0x85, &[0x04]).await?;
        self.command(0x86, &[0x00]).await?;
        self.command(0x87, &[0x00]).await?;
        self.command(0x88, &[0x48]).await?;
        self.command(0x89, &[0x00]).await?;
        self.command(0x8A, &[0x08]).await?;
        self.command(0x8B, &[0x02]).await?;
        self.command(0x8C, &[0xD8]).await?;
        self.command(0x8D, &[0x04]).await?;
        self.command(0x8E, &[0x00]).await?;
        self.command(0x8F, &[0x00]).await?;
        self.command(0x90, &[0x48]).await?;
        self.command(0x91, &[0x00]).await?;
        self.command(0x92, &[0x0A]).await?;
        self.command(0x93, &[0x02]).await?;
        self.command(0x94, &[0xDA]).await?;
        self.command(0x95, &[0x04]).await?;
        self.command(0x96, &[0x00]).await?;
        self.command(0x97, &[0x00]).await?;
        self.command(0x98, &[0x48]).await?;
        self.command(0x99, &[0x00]).await?;
        self.command(0x9A, &[0x0C]).await?;
        self.command(0x9B, &[0x02]).await?;
        self.command(0x9C, &[0xDC]).await?;
        self.command(0x9D, &[0x04]).await?;
        self.command(0x9E, &[0x00]).await?;
        self.command(0x9F, &[0x00]).await?;
        self.command(0xA0, &[0x48]).await?;
        self.command(0xA1, &[0x00]).await?;
        self.command(0xA2, &[0x05]).await?;
        self.command(0xA3, &[0x02]).await?;
        self.command(0xA4, &[0xD5]).await?;
        self.command(0xA5, &[0x04]).await?;
        self.command(0xA6, &[0x00]).await?;
        self.command(0xA7, &[0x00]).await?;
        self.command(0xA8, &[0x48]).await?;
        self.command(0xA9, &[0x00]).await?;
        self.command(0xAA, &[0x07]).await?;
        self.command(0xAB, &[0x02]).await?;
        self.command(0xAC, &[0xD7]).await?;
        self.command(0xAD, &[0x04]).await?;
        self.command(0xAE, &[0x00]).await?;
        self.command(0xAF, &[0x00]).await?;
        self.command(0xB0, &[0x48]).await?;
        self.command(0xB1, &[0x00]).await?;
        self.command(0xB2, &[0x09]).await?;
        self.command(0xB3, &[0x02]).await?;
        self.command(0xB4, &[0xD9]).await?;
        self.command(0xB5, &[0x04]).await?;
        self.command(0xB6, &[0x00]).await?;
        self.command(0xB7, &[0x00]).await?;
        self.command(0xB8, &[0x48]).await?;
        self.command(0xB9, &[0x00]).await?;
        self.command(0xBA, &[0x0B]).await?;
        self.command(0xBB, &[0x02]).await?;
        self.command(0xBC, &[0xDB]).await?;
        self.command(0xBD, &[0x04]).await?;
        self.command(0xBE, &[0x00]).await?;
        self.command(0xBF, &[0x00]).await?;
        self.command(0xC0, &[0x10]).await?;
        self.command(0xC1, &[0x47]).await?;
        self.command(0xC2, &[0x56]).await?;
        self.command(0xC3, &[0x65]).await?;
        self.command(0xC4, &[0x74]).await?;
        self.command(0xC5, &[0x88]).await?;
        self.command(0xC6, &[0x99]).await?;
        self.command(0xC7, &[0x01]).await?;
        self.command(0xC8, &[0xBB]).await?;
        self.command(0xC9, &[0xAA]).await?;
        self.command(0xD0, &[0x10]).await?;
        self.command(0xD1, &[0x47]).await?;
        self.command(0xD2, &[0x56]).await?;
        self.command(0xD3, &[0x65]).await?;
        self.command(0xD4, &[0x74]).await?;
        self.command(0xD5, &[0x88]).await?;
        self.command(0xD6, &[0x99]).await?;
        self.command(0xD7, &[0x01]).await?;
        self.command(0xD8, &[0xBB]).await?;
        self.command(0xD9, &[0xAA]).await?;
        self.command(0xF3, &[0x01]).await?;
        self.command(0xF0, &[0x00]).await?;
        self.command(0x21, &[0x00]).await?; // Inversion On
        self.command(0x11, &[0x00]).await?; // Sleep Out
        Timer::after_millis(120).await;
        self.command(0x29, &[0x00]).await?; // Display On
        self.command(0x36, &[0xC0]).await?; // MADCTL 180 deg (MY=1, MX=1), RGB order
        self.command(0x3A, &[0x55]).await?; // COLMOD 16-bit RGB565
        self.command(0x35, &[0x00]).await?; // Tearing effect line on
        self.command(0x44, &[0x01, 0xD1]).await?; // Set tear scanline
        self.command(0x53, &[0x20]).await?; // Write display control

        Ok(())
    }

    /// Sets PWM backlight duty cycle (0..=255) using perceptual quadratic curve.
    pub fn set_brightness(&mut self, level: u8) {
        let duty = if level == 0 {
            0
        } else {
            let num = (level as u32) * (level as u32) * 100;
            let den = 255 * 255;
            ((num / den) as u8).clamp(1, 100)
        };
        defmt::info!("Setting backlight brightness: level={}, duty={}%", level, duty);
        if let Err(e) = self.bl_channel.set_duty(duty) {
            defmt::error!("Failed to set backlight duty: {:?}", defmt::Debug2Format(&e));
        }
    }

    pub fn display_on(&mut self) {
        defmt::info!("Turning backlight on (100%)");
        let _ = self.bl_channel.set_duty(100);
    }

    pub fn display_off(&mut self) {
        defmt::info!("Turning backlight off (0%)");
        let _ = self.bl_channel.set_duty(0);
    }

    async fn command(&mut self, command: u8, parameters: &[u8]) -> Result<(), esp_hal::spi::Error> {
        let Port { spi, mut command_tx_buffer, framebuffer_descriptors } =
            self.port.take().unwrap();
        let len = parameters.len();
        if len > 0 {
            command_tx_buffer.as_mut_slice()[..len].copy_from_slice(parameters);
        }
        let mut transfer = match spi.half_duplex_write(
            DataMode::Single,
            Command::_8Bit(QSPI_CONTROL_OPCODE, DataMode::Single),
            Address::_24Bit((command as u32) << 8, DataMode::Single),
            0,
            len,
            command_tx_buffer,
        ) {
            Ok(transfer) => transfer,
            Err((e, spi, command_tx_buffer)) => {
                self.port = Some(Port { spi, command_tx_buffer, framebuffer_descriptors });
                return Err(e);
            }
        };
        transfer.wait_for_done().await;
        let (spi, command_tx_buffer) = transfer.wait();
        self.port = Some(Port { spi, command_tx_buffer, framebuffer_descriptors });
        Ok(())
    }
}

/// Background worker task that exclusively owns the SH8601 display and processes flush jobs.
#[embassy_executor::task]
pub async fn display_task(mut display: Sh8601) {
    if let Err(e) = display.init().await {
        defmt::error!("Failed to initialize SH8601 display: {}", e);
    } else {
        defmt::info!("SH8601 display initialized successfully");
    }

    let mut perf_tracker = app_shell::PerfTracker::new();
    let loop_start_time = Instant::now();

    loop {
        match DISPLAY_COMMAND_CHANNEL.receive().await {
            DisplayCommand::Flush(job) => {
                let t_start = cycle_count();
                for rect in &job.rects {
                    let mut session = match display
                        .start_session(rect.x, rect.y, rect.width, rect.height)
                        .await
                    {
                        Ok(s) => s,
                        Err(e) => {
                            defmt::error!("Failed to start session: {:?}", defmt::Debug2Format(&e));
                            break;
                        }
                    };

                    debug_assert_eq!(rect.x, 0, "Partial row X offset is not supported");
                    debug_assert_eq!(
                        rect.width, RENDER_WIDTH,
                        "Partial row width is not supported"
                    );

                    let bytes_per_row =
                        rect.width as usize * core::mem::size_of::<BigEndianRgb565>();
                    let mut row = rect.y;
                    while row < rect.y + rect.height {
                        let remaining_rows = (rect.y + rect.height - row) as usize;
                        let chunk_rows = remaining_rows.min(MAX_CHUNK_ROWS);
                        let start_byte = (row as usize * RENDER_STRIDE + rect.x as usize) * 2;
                        let chunk_bytes = chunk_rows * bytes_per_row;

                        let pixel_slice: &'static mut [u8] = unsafe {
                            core::slice::from_raw_parts_mut(
                                (job.fb.0.as_mut_ptr() as *mut u8).add(start_byte),
                                chunk_bytes,
                            )
                        };

                        if let Err(e) = session.send_pixel_chunk(pixel_slice).await {
                            defmt::error!("Session send error: {:?}", defmt::Debug2Format(&e));
                            break;
                        }
                        row += chunk_rows as u16;
                    }
                }
                let transfer_cycles = cycle_count().wrapping_sub(t_start);
                let _ = FLUSH_RETURN_CHANNEL.send(job.fb).await;

                if transfer_cycles > 0 {
                    let dirty_pixels: u32 =
                        job.rects.iter().map(|r| r.width as u32 * r.height as u32).sum();
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
                display.set_brightness(level);
            }
            DisplayCommand::DisplayOff => {
                display.display_off();
            }
            DisplayCommand::DisplayOn => {
                display.display_on();
            }
        }
    }
}
