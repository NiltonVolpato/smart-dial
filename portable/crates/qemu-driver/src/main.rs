// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Integration checks for the co-processor, over its Tether UART: QEMU's PTY
//! (see coprocessor/qemu_app/test-qemu.sh) or a serial port.
//!
//! Environment:
//! - TETHER_UART: the PTY or serial device (required).
//! - TETHER_UART_BAUD: its baud rate; 0 (the default) for a PTY.
//! - SONOS_SEED_IP: a real speaker to run the Sonos checks against. Unset,
//!   they're skipped.

mod connector;

use std::env;
use std::error::Error;
use std::net::Ipv4Addr;
use std::time::Duration;

use connector::Connector;
use coprocessor::{proto, sonos, system};

type Result<T = ()> = std::result::Result<T, Box<dyn Error>>;

fn main() -> Result {
    let uart =
        env::var("TETHER_UART").map_err(|_| "TETHER_UART must name the PTY or serial port")?;
    let baud = match env::var("TETHER_UART_BAUD") {
        Ok(baud) => baud.parse()?,
        Err(_) => 0,
    };

    println!("=== Connecting to {uart} ===");
    let mut connector = Connector::connect(&uart, baud)?;
    println!("=== Tether link established ===");

    check_system_watch(&mut connector)?;
    check_report_battery(&mut connector)?;
    match env::var("SONOS_SEED_IP") {
        Ok(seed) => check_sonos(&mut connector, seed.parse()?)?,
        Err(_) => println!("SONOS_SEED_IP unset: skipping the real-speaker checks"),
    }
    check_unreachable_seed(&mut connector)?;

    println!("=== All QEMU integration checks PASSED ===");
    Ok(())
}

/// The initial burst, then SNTP's sync.
fn check_system_watch(connector: &mut Connector) -> Result {
    println!("Testing SystemService.Watch...");
    let mut watch = system::Client(&connector.client).watch(&proto::EmptyT::default(), 4);

    let mut connected = false;
    let mut provisioning = None;
    let mut synced_at = None;
    connector.pump_until(Duration::from_secs(30), |_| {
        while let Some(msg) = watch.try_recv() {
            let event = msg.get();
            if let Some(status) = event.event_as_wifi_status() {
                println!(
                    "  WifiStatus: state={:?}, ip={}, ssid={:?}, rssi={}",
                    status.state(),
                    Ipv4Addr::from(status.ip()),
                    status.ssid(),
                    status.rssi()
                );
                // Over QEMU's Ethernet there's no SSID: an IP is what counts.
                connected = status.state() == proto::WifiState::Connected && status.ip() != 0;
            } else if let Some(status) = event.event_as_provisioning_status() {
                println!("  ProvisioningStatus: state={:?}", status.state());
                provisioning = Some(status.state());
            } else if let Some(ts) = event.event_as_time_synced().and_then(|t| t.timestamp()) {
                println!(
                    "  TimeSynced: epoch={}, subsec={}",
                    ts.epoch_seconds(),
                    ts.subsec_micros()
                );
                synced_at = Some(ts.epoch_seconds());
            }
        }
        connected && synced_at.is_some()
    })?;
    assert_eq!(provisioning, Some(proto::ProvisioningState::Inactive), "QEMU has no BLE");
    // 2026-01-01: an unsynced clock starts at 1970.
    assert!(synced_at.is_some_and(|s| s >= 1_767_225_600), "Implausible synced time");
    Ok(())
}

fn check_report_battery(connector: &mut Connector) -> Result {
    println!("Testing SystemService.ReportBattery...");
    let mut status = proto::BatteryStatusT::default();
    status.millivolts = 4200;
    status.percent = 100;
    status.is_plugged = true;
    let call = system::Client(&connector.client).report_battery(&status);
    let res = connector
        .wait_call(call, Duration::from_secs(5))
        .map_err(|s| format!("ReportBattery failed with status: {s:?}"))?;
    println!("  ReportBattery result: {:?}", res.get());
    Ok(())
}

/// Read-only: topology from `seed`, then a summary of its groups and the
/// first one's playback.
fn check_sonos(connector: &mut Connector, seed: Ipv4Addr) -> Result {
    println!("Testing SonosService.WatchTopology with seed {seed}...");
    let mut request = proto::TopologyRequestT::default();
    request.seed_ip = u32::from(seed);
    let mut topology = sonos::Client(&connector.client).watch_topology(&request, 16);
    let mut coordinator_ips = Vec::new();
    connector.pump_until(Duration::from_secs(15), |_| {
        if let Some(msg) = topology.try_recv() {
            let update = msg.get();
            if let Some(g) = update.group() {
                println!(
                    "  Sonos Group: name={:?}, coord_ip={}, playing={}",
                    g.name(),
                    Ipv4Addr::from(g.coordinator_ip()),
                    g.is_playing()
                );
                coordinator_ips.push(g.coordinator_ip());
            }
            return !coordinator_ips.is_empty()
                && coordinator_ips.len() >= usize::from(update.total_groups());
        }
        false
    })?;
    println!("  Discovered {} Sonos group(s)", coordinator_ips.len());

    // The reactor's HTTP client, permits and XML queries.
    println!("Testing SonosService.GetGroupsSummary...");
    let mut request = proto::GroupsSummaryRequestT::default();
    request.coordinator_ips = Some(coordinator_ips.clone());
    let call = sonos::Client(&connector.client).get_groups_summary(&request);
    let summary = connector
        .wait_call(call, Duration::from_secs(10))
        .map_err(|s| format!("GetGroupsSummary failed with status: {s:?}"))?;
    let groups: Vec<_> = summary.get().groups().into_iter().flatten().collect();
    for g in &groups {
        println!(
            "  Summary: coord_ip={}, playing={}, title={:?}",
            Ipv4Addr::from(g.coordinator_ip()),
            g.is_playing(),
            g.title()
        );
    }
    let answered: Vec<u32> = groups.iter().map(|g| g.coordinator_ip()).collect();
    assert_eq!(answered, coordinator_ips, "Summary must answer every group, in order");

    // A GENA subscription, whose first NOTIFY carries the state.
    let coordinator = coordinator_ips[0];
    println!("Testing SonosService.WatchPlayback on {}...", Ipv4Addr::from(coordinator));
    let mut request = proto::PlaybackRequestT::default();
    request.endpoint_ip = coordinator;
    let mut playback = sonos::Client(&connector.client).watch_playback(&request, 4);
    connector.pump_until(Duration::from_secs(15), |_| {
        let Some(msg) = playback.try_recv() else { return false };
        let update = msg.get();
        println!(
            "  StateUpdate: endpoint={}, state={:?}, volume={:?}, muted={:?}, title={:?}",
            Ipv4Addr::from(update.endpoint_ip()),
            update.transport_state(),
            update.volume(),
            update.is_muted(),
            update.track().and_then(|t| t.title())
        );
        assert_eq!(update.endpoint_ip(), coordinator);
        true
    })?;
    Ok(())
}

/// A seed nobody answers at: the stream ends with an error.
fn check_unreachable_seed(connector: &mut Connector) -> Result {
    println!("Testing SonosService.WatchTopology error reporting with invalid seed...");
    let mut request = proto::TopologyRequestT::default();
    request.seed_ip = u32::from(Ipv4Addr::new(10, 0, 2, 254));
    let topology = sonos::Client(&connector.client).watch_topology(&request, 16);
    connector.pump_until(Duration::from_secs(15), |_| {
        let Some(end) = topology.end() else { return false };
        println!("  Stream ended with: {end:?}");
        assert!(end.is_err(), "An unreachable seed must end the stream with an error");
        true
    })?;
    Ok(())
}
