// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Inter-MCU Tether RPC communication with the onboard ESP32 co-processor.
//!
//! The link is driven by two ring buffers plus UART1 interrupts:
//!
//! - RX: the ISR drains the 128-byte hardware FIFO into `RX_QUEUE`; the async
//!   `RingBufferRx` drains `RX_QUEUE` for `run_client`.
//! - TX: `RingBufferTx` copies outgoing frames into `TX_QUEUE`; the ISR refills
//!   the hardware FIFO from `TX_QUEUE` whenever it drops below 32 bytes.
//!
//! This keeps the async executor from ever blocking on UART I/O.

use alloc::rc::Rc;
use alloc::string::ToString;
use core::net::Ipv4Addr;
use core::sync::atomic::{AtomicUsize, Ordering};

use bbqueue::nicknames::Churrasco;
use coprocessor::proto::SystemEventKind;
use coprocessor::{proto, system, wifi};
use defmt::{error, info, warn};
use embassy_sync::waitqueue::AtomicWaker;
use embassy_time::Timer;
use embedded_io_async::{ErrorType, Read, Write};
use esp_hal::interrupt::Priority;
use esp_hal::peripherals::{GPIO38, GPIO48, UART1};
use esp_hal::uart::{Config, RxConfig, TxConfig, Uart, UartInterrupt};
use tether_core::client::{Client, SharedClient};
use tether_core::link::LinkConfig;
use tether_embassy::run_client;

const RX_RING_BUFFER_SIZE: usize = 2048;
const TX_RING_BUFFER_SIZE: usize = 2048;
static RX_QUEUE: Churrasco<RX_RING_BUFFER_SIZE> = Churrasco::new();
static TX_QUEUE: Churrasco<TX_RING_BUFFER_SIZE> = Churrasco::new();
static RX_WAKER: AtomicWaker = AtomicWaker::new();
static TX_WAKER: AtomicWaker = AtomicWaker::new();
static DROPPED_BYTES: AtomicUsize = AtomicUsize::new(0);

struct RingBufferRx;

impl ErrorType for RingBufferRx {
    type Error = core::convert::Infallible;
}

impl Read for RingBufferRx {
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
        if buf.is_empty() {
            return Ok(0);
        }
        let consumer = RX_QUEUE.stream_consumer();
        core::future::poll_fn(|cx| {
            if let Ok(grant) = consumer.read() {
                let to_copy = buf.len().min(grant.len());
                buf[..to_copy].copy_from_slice(&grant[..to_copy]);
                grant.release(to_copy);
                core::task::Poll::Ready(Ok(to_copy))
            } else {
                RX_WAKER.register(cx.waker());
                if let Ok(grant) = consumer.read() {
                    let to_copy = buf.len().min(grant.len());
                    buf[..to_copy].copy_from_slice(&grant[..to_copy]);
                    grant.release(to_copy);
                    core::task::Poll::Ready(Ok(to_copy))
                } else {
                    core::task::Poll::Pending
                }
            }
        })
        .await
    }
}

/// UART1 interrupt handler.
///
/// We drive the peripheral at the register level because esp-hal does not expose
/// the TX-FIFO-empty interrupt in its public `UartInterrupt` enum. The three
/// relevant registers are:
///
/// - `int_raw` / `int_st`: read pending interrupt flags.
/// - `int_ena`: interrupt mask — `set_bit()` enables, `clear_bit()` disables.
/// - `int_clr`: write 1 to a bit to acknowledge (clear) the matching flag.
#[esp_hal::handler(priority = Priority::Priority2)]
fn uart1_isr() {
    let uart = unsafe { &*UART1::ptr() };
    let raw = uart.int_raw().read();

    // --- RX handling ---
    if raw.rxfifo_full().bit_is_set()
        || raw.rxfifo_tout().bit_is_set()
        || raw.rxfifo_ovf().bit_is_set()
    {
        let count = (uart.status().read().rxfifo_cnt().bits() as usize).min(128);
        if count > 0 {
            let producer = RX_QUEUE.stream_producer();
            if let Ok(mut grant) = producer.grant_exact(count) {
                for slot in &mut grant[..count] {
                    *slot = uart.fifo().read().rxfifo_rd_byte().bits();
                }
                grant.commit(count);
                RX_WAKER.wake();
            } else {
                // Drop overflow bytes from hardware FIFO so FIFO doesn't lock up
                for _ in 0..count {
                    let _ = uart.fifo().read().rxfifo_rd_byte().bits();
                }
                DROPPED_BYTES.fetch_add(count, Ordering::Relaxed);
            }
        }

        let overflow = uart.int_raw().read().rxfifo_ovf().bit_is_set();
        // Acknowledge the RX interrupts we handled.
        uart.int_clr().write(|w| {
            if overflow {
                w.rxfifo_ovf().clear_bit_by_one();
            }
            w.rxfifo_full().clear_bit_by_one().rxfifo_tout().clear_bit_by_one()
        });
    }

    // --- TX handling ---
    if raw.txfifo_empty().bit_is_set() {
        let consumer = TX_QUEUE.stream_consumer();

        loop {
            let tx_count = uart.status().read().txfifo_cnt().bits() as usize;
            let free = 128usize.saturating_sub(tx_count);
            if free == 0 {
                break;
            }

            let Ok(grant) = consumer.read() else {
                // Nothing left to send — disable the interrupt until the writer
                // re-enables it after adding more data.
                uart.int_ena().modify(|_, w| w.txfifo_empty().clear_bit());
                break;
            };

            let to_write = grant.len().min(free);
            for byte in &grant[..to_write] {
                uart.fifo().write(|w| unsafe { w.rxfifo_rd_byte().bits(*byte) });
            }
            grant.release(to_write);
        }

        // Wake any writer that was waiting for queue space.
        TX_WAKER.wake();
        // Acknowledge the TX interrupt so it can fire again when the FIFO drops
        // back below the threshold.
        uart.int_clr().write(|w| w.txfifo_empty().clear_bit_by_one());
    }
}

struct RingBufferTx;

impl ErrorType for RingBufferTx {
    type Error = core::convert::Infallible;
}

impl Write for RingBufferTx {
    async fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
        if buf.is_empty() {
            return Ok(0);
        }

        let mut written = 0;
        while written < buf.len() {
            let producer = TX_QUEUE.stream_producer();
            match producer.grant_max_remaining(buf.len() - written) {
                Ok(mut grant) => {
                    let to_copy = grant.len();
                    grant[..to_copy].copy_from_slice(&buf[written..written + to_copy]);
                    grant.commit(to_copy);
                    written += to_copy;

                    // Unmask the TX-FIFO-empty interrupt so the ISR starts (or
                    // continues) moving bytes from the software queue into the
                    // hardware FIFO. This is a read-modify-write on `int_ena`;
                    // only the `txfifo_empty` bit is contended with the ISR, and
                    // any race is self-correcting because the ISR re-evaluates the
                    // queue on every fire.
                    let uart = unsafe { &*UART1::ptr() };
                    uart.int_ena().modify(|_, w| w.txfifo_empty().set_bit());
                }
                Err(_) => {
                    core::future::poll_fn(|cx| {
                        TX_WAKER.register(cx.waker());
                        let producer = TX_QUEUE.stream_producer();
                        if producer.grant_max_remaining(buf.len() - written).is_ok() {
                            core::task::Poll::Ready(())
                        } else {
                            core::task::Poll::Pending
                        }
                    })
                    .await;
                }
            }
        }
        Ok(written)
    }

    async fn flush(&mut self) -> Result<(), Self::Error> {
        core::future::poll_fn(|cx| {
            if TX_QUEUE.stream_consumer().read().is_err() {
                core::task::Poll::Ready(())
            } else {
                TX_WAKER.register(cx.waker());
                core::task::Poll::Pending
            }
        })
        .await;
        Ok(())
    }
}

/// Shared client transport type for the co-processor RPC link.
pub type CoprocessorTransport = alloc::rc::Rc<SharedClient>;
pub type WifiClient = wifi::Client<CoprocessorTransport>;
pub type SystemClient = system::Client<CoprocessorTransport>;

/// Creates a new shared Tether RPC client instance for the onboard co-processor.
pub fn create_shared_client(boot_id: u32) -> CoprocessorTransport {
    let link_config = LinkConfig {
        baud_rate: 921_600,
        max_frame: coprocessor::proto::Limits::MaxFrame.0 as usize,
        ..Default::default()
    };
    alloc::rc::Rc::new(SharedClient::new(Client::new(boot_id, link_config)))
}

#[embassy_executor::task]
pub async fn coprocessor_task(
    uart: UART1<'static>,
    tx_pin: GPIO38<'static>,
    rx_pin: GPIO48<'static>,
    client: CoprocessorTransport,
) {
    info!("[COPROCESSOR] Initializing UART1 at 921600 baud with Churrasco BBQueue ISR...");

    let uart_config = Config::default()
        .with_baudrate(921_600)
        .with_rx(RxConfig::default().with_fifo_full_threshold(96))
        .with_tx(TxConfig::default().with_fifo_empty_threshold(32));
    let mut uart = match Uart::new(uart, uart_config) {
        Ok(u) => u.with_tx(tx_pin).with_rx(rx_pin),
        Err(e) => {
            error!("[COPROCESSOR] Failed to initialize UART1: {:?}", defmt::Debug2Format(&e));
            return;
        }
    };

    uart.set_interrupt_handler(uart1_isr);
    uart.listen(UartInterrupt::RxFifoFull | UartInterrupt::RxTimeout);

    match run_client(&client, RingBufferRx, RingBufferTx).await {
        Ok(state) => {
            warn!("[COPROCESSOR] Link ended: {:?}, rebooting S3...", defmt::Debug2Format(&state))
        }
        Err(err) => {
            error!("[COPROCESSOR] Link error: {:?}, rebooting S3...", defmt::Debug2Format(&err))
        }
    }

    esp_hal::system::software_reset();
}

/// Follows the co-processor's system events: Wi-Fi state for the system menu,
/// and SNTP syncs for the RTC.
#[embassy_executor::task]
pub async fn system_task(
    system_client: SystemClient,
    wifi: Rc<common::WifiSignal>,
    provisioning: Rc<common::ProvisioningSignal>,
) {
    loop {
        let mut stream = system_client.watch(&proto::EmptyT::default(), 2);
        while let Some(msg) = stream.recv().await {
            let event = msg.get();
            match event.event_type() {
                SystemEventKind::WifiStatus => {
                    on_wifi_status(&event.event_as_wifi_status().unwrap(), &wifi)
                }
                SystemEventKind::TimeSynced => {
                    let ts = event.event_as_time_synced().and_then(|t| t.timestamp()).unwrap();
                    crate::bsp::rtc::sync_rtc(ts.epoch_seconds(), ts.subsec_micros());
                    info!(
                        "[COPROCESSOR] Time synced: epoch={}, subsec={}",
                        ts.epoch_seconds(),
                        ts.subsec_micros()
                    );
                }
                SystemEventKind::ProvisioningStatus => {
                    let state = event.event_as_provisioning_status().unwrap().state();
                    info!("[COPROCESSOR] Provisioning: {}", state.variant_name().unwrap_or("?"));
                    provisioning.signal(state == proto::ProvisioningState::Active);
                }
                _ => {}
            };
        }
        warn!("[COPROCESSOR] System watch ended, retrying in 500ms...");
        Timer::after_millis(500).await;
    }
}

fn on_wifi_status(status: &proto::WifiStatus<'_>, wifi: &common::WifiSignal) {
    let state = match status.state() {
        proto::WifiState::Connecting => theme::WifiState::Connecting,
        proto::WifiState::Connected => theme::WifiState::Connected,
        proto::WifiState::Failed => theme::WifiState::Failed,
        _ => theme::WifiState::Disconnected,
    };
    wifi.signal(common::Wifi { state, ssid: status.ssid().unwrap_or("").into() });
    info!(
        "[COPROCESSOR] Wi-Fi: state={}, ssid={}, ip={}, rssi={}",
        status.state().variant_name().unwrap_or("?"),
        status.ssid().unwrap_or(""),
        Ipv4Addr::from_bits(status.ip()).to_string().as_str(),
        status.rssi()
    );
}
