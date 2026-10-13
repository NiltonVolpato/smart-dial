// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! A Tether link to the co-processor over a serial port or PTY: QEMU's UART,
//! or the hardware's.

use std::io::{ErrorKind, Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant, SystemTime};

use tether::{Message, Table};
use tether_core::client::{Client, SharedClient};
use tether_core::link::{LinkConfig, LinkState};

pub(crate) struct Connector {
    port: Box<dyn serialport::SerialPort>,
    received: mpsc::Receiver<Vec<u8>>,
    stop: Arc<AtomicBool>,
    reader: Option<std::thread::JoinHandle<()>>,
    pub(crate) client: std::rc::Rc<SharedClient>,
    start: Instant,
}

impl Connector {
    /// Opens the serial device or PTY at `path` and establishes a Tether link.
    pub(crate) fn connect(path: &str, baud: u32) -> Result<Self, Box<dyn std::error::Error>> {
        let deadline = Instant::now() + Duration::from_secs(60);
        let port = loop {
            match serialport::new(path, baud).timeout(Duration::from_millis(10)).open() {
                Ok(port) => break port,
                Err(_e) if Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(200));
                }
                Err(e) => return Err(format!("can't open {path}: {e}").into()),
            }
        };

        let (tx, received) = mpsc::channel();
        let stop = Arc::new(AtomicBool::new(false));
        let reader = {
            let mut port = port.try_clone()?;
            let stop = stop.clone();
            let path = path.to_owned();
            std::thread::spawn(move || {
                let mut buf = [0; 512];
                while !stop.load(Ordering::Relaxed) {
                    match port.read(&mut buf) {
                        Ok(n) => {
                            if tx.send(buf[..n].to_vec()).is_err() {
                                break;
                            }
                        }
                        Err(e) if matches!(e.kind(), ErrorKind::TimedOut) => {}
                        Err(e) => {
                            eprintln!("Reading {path} failed: {e}");
                            break;
                        }
                    }
                }
            })
        };

        let nanos = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos();
        let config = LinkConfig {
            baud_rate: baud,
            max_frame: coprocessor::proto::Limits::MaxFrame.0 as usize,
            ..LinkConfig::default()
        };
        let client = std::rc::Rc::new(SharedClient::new(Client::new(nanos as u32 | 1, config)));

        let mut connector =
            Self { port, received, stop, reader: Some(reader), client, start: Instant::now() };

        connector.pump_until(Duration::from_secs(60), |c| {
            matches!(c.client.link_state(), LinkState::Linked { .. })
        })?;

        Ok(connector)
    }

    fn now(&self) -> u64 {
        self.start.elapsed().as_millis() as u64
    }

    fn write(&mut self, mut bytes: &[u8]) {
        let deadline = Instant::now() + Duration::from_secs(20);
        while !bytes.is_empty() {
            assert!(Instant::now() < deadline, "the device isn't reading");
            match self.port.write(bytes) {
                Ok(n) => bytes = &bytes[n..],
                Err(e) if matches!(e.kind(), ErrorKind::TimedOut) => {}
                Err(e) => panic!("{e}"),
            }
        }
    }

    fn pump_once(&mut self) {
        let state = self.client.link_state();
        assert!(!state.is_terminal(), "link terminal: {state:?}");

        loop {
            let wire = self.client.poll_transmit(self.now());
            let Some(wire) = wire else { break };
            self.write(&wire);
        }

        match self.received.recv_timeout(Duration::from_millis(5)) {
            Ok(bytes) => self.client.receive(&bytes),
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(e) => panic!("the reader stopped: {e}"),
        }
        while let Ok(bytes) = self.received.try_recv() {
            self.client.receive(&bytes);
        }
    }

    pub(crate) fn pump_until(
        &mut self,
        timeout: Duration,
        mut done: impl FnMut(&mut Self) -> bool,
    ) -> Result<(), &'static str> {
        let deadline = Instant::now() + timeout;
        while !done(self) {
            if Instant::now() >= deadline {
                return Err("timed out waiting for condition");
            }
            self.pump_once();
        }
        Ok(())
    }

    pub(crate) fn wait_call<T: Table>(
        &mut self,
        mut call: tether::Call<tether_core::client::Call, T>,
        timeout: Duration,
    ) -> Result<Message<T>, tether::Status> {
        let mut result = None;
        let _ = self.pump_until(timeout, |_| {
            result = call.try_result();
            result.is_some()
        });
        result.unwrap_or(Err(tether::Status::DeadlineExceeded))
    }
}

impl Drop for Connector {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}
