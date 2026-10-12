// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

extern crate std;
use std::cell::RefCell;
use std::collections::{BTreeMap, VecDeque};
use std::task::{Context, Poll};
use std::vec;
use std::vec::Vec;

use tether::{MethodId, Pack, RawCall, RawChannel, Status, Table, Transport};

use super::*;

// --- In-Memory Fake Transport for testing ---

type Stream = (Vec<Vec<u8>>, Result<(), Status>);

#[derive(Default)]
struct FakeTransport {
    requests: RefCell<Vec<(MethodId, Vec<u8>)>>,
    responses: RefCell<BTreeMap<MethodId, Result<Vec<u8>, Status>>>,
    channels: RefCell<BTreeMap<MethodId, Stream>>,
}

impl FakeTransport {
    fn respond(&self, method: MethodId, result: Result<&impl Pack, Status>) {
        self.responses.borrow_mut().insert(method, result.map(Pack::to_bytes));
    }

    fn stream(&self, method: MethodId, items: Vec<Vec<u8>>, end: Result<(), Status>) {
        self.channels.borrow_mut().insert(method, (items, end));
    }
}

impl Transport for FakeTransport {
    type Call = ReadyCall;
    type Channel = QueuedChannel;

    fn call(&self, method: MethodId, request: &[u8], _timeout_ms: u32) -> ReadyCall {
        self.requests.borrow_mut().push((method, request.to_vec()));
        let result = self.responses.borrow().get(&method).cloned();
        ReadyCall(Some(result.unwrap_or(Err(Status::Unimplemented))))
    }

    fn open(&self, method: MethodId, request: &[u8], _capacity: u16) -> QueuedChannel {
        self.requests.borrow_mut().push((method, request.to_vec()));
        let (items, end) = self
            .channels
            .borrow()
            .get(&method)
            .cloned()
            .unwrap_or((vec![], Err(Status::Unimplemented)));
        QueuedChannel { items: items.into(), end }
    }
}

struct ReadyCall(Option<Result<Vec<u8>, Status>>);

impl RawCall for ReadyCall {
    type Buf = Vec<u8>;

    fn poll_result(&mut self, _: &mut Context<'_>) -> Poll<Result<Vec<u8>, Status>> {
        Poll::Ready(self.0.take().expect("polled after completion"))
    }
}

struct QueuedChannel {
    items: VecDeque<Vec<u8>>,
    end: Result<(), Status>,
}

impl RawChannel for QueuedChannel {
    type Buf = Vec<u8>;

    fn poll_recv(&mut self, _: &mut Context<'_>) -> Poll<Option<Vec<u8>>> {
        Poll::Ready(self.items.pop_front())
    }

    fn end(&self) -> Option<Result<(), Status>> {
        self.items.is_empty().then_some(self.end)
    }
}

// --- Tests ---

#[test]
fn test_time_synced_event_pack_verify() {
    let event = proto::SystemEventT {
        event: proto::SystemEventKindT::TimeSynced(alloc::boxed::Box::new(proto::TimeSyncedT {
            timestamp: Some(proto::TimestampT { epoch_seconds: 1775215000, subsec_micros: 987654 }),
        })),
    };
    let bytes = event.to_bytes();
    let view = proto::SystemEvent::verify(&bytes).expect("Verification must succeed");
    assert_eq!(view.event_type(), proto::SystemEventKind::TimeSynced);
    let ts = view
        .event_as_time_synced()
        .and_then(|t| t.timestamp())
        .expect("timestamp present");
    assert_eq!(ts.epoch_seconds(), 1775215000);
    assert_eq!(ts.subsec_micros(), 987654);
}

#[test]
fn test_battery_status_pack_verify() {
    let batt_obj = proto::BatteryStatusT { millivolts: 4180, percent: 95, is_plugged: true };
    let bytes = batt_obj.to_bytes();
    let view = proto::BatteryStatus::verify(&bytes).expect("Verification must succeed");
    assert_eq!(view.millivolts(), 4180);
    assert_eq!(view.percent(), 95);
    assert!(view.is_plugged());
}

#[test]
fn test_wifi_client_connect_rpc() {
    let transport = FakeTransport::default();
    transport.respond(wifi::CONNECT, Ok(&proto::EmptyT {}));

    let client = wifi::Client(&transport);
    let mut call = client.connect(&proto::WifiConnectRequestT {
        ssid: Some("OfficeNet".into()),
        password: Some("secret123".into()),
    });

    let res = call.try_result();
    assert!(res.is_some());
    assert!(res.unwrap().is_ok());

    let reqs = transport.requests.borrow();
    assert_eq!(reqs.len(), 1);
    assert_eq!(reqs[0].0, wifi::CONNECT);
    let req_view = proto::WifiConnectRequest::verify(&reqs[0].1).unwrap();
    assert_eq!(req_view.ssid(), Some("OfficeNet"));
    assert_eq!(req_view.password(), Some("secret123"));
}

#[test]
fn test_system_client_watch_stream() {
    let transport = FakeTransport::default();
    let wifi_event = |state, ip, ssid: Option<&str>, rssi| {
        proto::SystemEventT {
            event: proto::SystemEventKindT::WifiStatus(alloc::boxed::Box::new(
                proto::WifiStatusT { state, ip, ssid: ssid.map(Into::into), rssi },
            )),
        }
        .to_bytes()
    };
    let item1 = wifi_event(proto::WifiState::Connecting, 0, Some("OfficeNet"), 0);
    let item2 = wifi_event(proto::WifiState::Connected, 0x0A00002A, Some("OfficeNet"), -50);

    transport.stream(system::WATCH, vec![item1, item2], Ok(()));

    let client = system::Client(&transport);
    let mut stream = client.watch(&proto::EmptyT {}, 4);

    let first = stream.try_recv().expect("first item present");
    let status = first.get().event_as_wifi_status().expect("a Wi-Fi status");
    assert_eq!(status.state(), proto::WifiState::Connecting);

    let second = stream.try_recv().expect("second item present");
    let status = second.get().event_as_wifi_status().expect("a Wi-Fi status");
    assert_eq!(status.state(), proto::WifiState::Connected);
    assert_eq!(status.ip(), 0x0A00002A);
    assert_eq!(status.ssid(), Some("OfficeNet"));

    assert!(stream.try_recv().is_none());
    assert_eq!(stream.end(), Some(Ok(())));
}

#[test]
fn test_system_client_report_battery() {
    let transport = FakeTransport::default();
    transport.respond(system::REPORT_BATTERY, Ok(&proto::EmptyT {}));

    let client = system::Client(&transport);
    let mut call = client.report_battery(&proto::BatteryStatusT {
        millivolts: 3950,
        percent: 78,
        is_plugged: false,
    });

    let res = call.try_result();
    assert!(res.is_some());
    assert!(res.unwrap().is_ok());

    let reqs = transport.requests.borrow();
    assert_eq!(reqs.len(), 1);
    assert_eq!(reqs[0].0, system::REPORT_BATTERY);
    let req_view = proto::BatteryStatus::verify(&reqs[0].1).unwrap();
    assert_eq!(req_view.millivolts(), 3950);
    assert_eq!(req_view.percent(), 78);
    assert!(!req_view.is_plugged());
}

#[test]
fn test_sonos_topology_pack_verify() {
    let member = proto::MemberT {
        name: Some("Living Room".into()),
        uuid: Some("RINCON_123".into()),
        ip: 0xC0A80164,
    };
    let group = proto::GroupT {
        id: Some("RINCON_123:456".into()),
        name: Some("Living Room".into()),
        coordinator_ip: 0xC0A80164,
        is_playing: true,
        members: Some(alloc::vec![member]),
    };
    let topo = proto::TopologyUpdateT {
        sequence: 1,
        group_index: 0,
        total_groups: 1,
        group: Some(alloc::boxed::Box::new(group)),
    };

    let bytes = topo.to_bytes();
    let view = proto::TopologyUpdate::verify(&bytes).expect("Verification must succeed");
    assert_eq!(view.sequence(), 1);
    assert_eq!(view.group_index(), 0);
    assert_eq!(view.total_groups(), 1);
    let g = view.group().expect("group present");
    assert_eq!(g.name(), Some("Living Room"));
    assert_eq!(g.coordinator_ip(), 0xC0A80164);
    assert!(g.is_playing());
}

#[test]
fn test_sonos_state_update_pack_verify() {
    let track = proto::TrackMetadataT {
        title: Some("The Rope (feat. POLIÇA)".into()),
        artist: Some("Lane 8".into()),
        album: Some("Brightest Lights".into()),
        duration_seconds: Some(295),
        elapsed_seconds: Some(42),
        album_art_uri: Some("/getaa?s=1&u=x-sonos-http%3a...".into()),
    };
    let state = proto::StateUpdateT {
        endpoint_ip: 0xC0A80169,
        transport_state: Some(proto::TransportState::Playing),
        volume: Some(15),
        is_muted: Some(false),
        track: Some(alloc::boxed::Box::new(track)),
    };

    let bytes = state.to_bytes();
    let view = proto::StateUpdate::verify(&bytes).expect("Verification must succeed");
    assert_eq!(view.endpoint_ip(), 0xC0A80169);
    assert_eq!(view.transport_state(), Some(proto::TransportState::Playing));
    assert_eq!(view.volume(), Some(15));
    assert_eq!(view.is_muted(), Some(false));
    let t = view.track().expect("track present");
    assert_eq!(t.title(), Some("The Rope (feat. POLIÇA)"));
    assert_eq!(t.artist(), Some("Lane 8"));
    assert_eq!(t.album(), Some("Brightest Lights"));
    assert_eq!(t.duration_seconds(), Some(295));
    assert_eq!(t.elapsed_seconds(), Some(42));
}

#[test]
fn test_sonos_client_action_and_volume() {
    let transport = FakeTransport::default();
    transport.respond(sonos::ACTION, Ok(&proto::EmptyT {}));
    transport.respond(sonos::SET_VOLUME, Ok(&proto::EmptyT {}));

    let client = sonos::Client(&transport);
    let action_req = proto::ActionRequestT {
        endpoint_ip: 0xC0A80164,
        action: proto::ActionType::TogglePlayPause,
    };
    let mut action_call = client.action(&action_req);
    assert!(action_call.try_result().unwrap().is_ok());

    let vol_req = proto::VolumeRequestT { endpoint_ip: 0xC0A80164, volume: 5, is_relative: true };
    let mut vol_call = client.set_volume(&vol_req);
    assert!(vol_call.try_result().unwrap().is_ok());

    let reqs = transport.requests.borrow();
    assert_eq!(reqs.len(), 2);
    assert_eq!(reqs[0].0, sonos::ACTION);
    assert_eq!(reqs[1].0, sonos::SET_VOLUME);
}
