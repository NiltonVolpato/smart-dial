// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::collections::BTreeMap;
use alloc::rc::Rc;
use alloc::string::String;
use alloc::vec::Vec;
use core::any::Any;
use core::cell::RefCell;
use core::fmt::Write;
use core::pin::Pin;

use embassy_futures::select::{Either, Either4, select, select4};
use embassy_sync::blocking_mutex::raw::NoopRawMutex;
use embassy_sync::channel::Channel;
use slint::Model;

slint::include_modules!();

/// Default shared Tether client transport for the Sonos application.
pub type SonosTransport = alloc::rc::Rc<tether_core::client::SharedClient>;

#[derive(Clone)]
pub struct SonosAppFactory<T: tether::Transport + Clone + 'static = SonosTransport> {
    transport: Option<T>,
    art: Option<album_art::Decoder>,
    info: theme::AppInfo,
}

impl<T: tether::Transport + Clone + 'static> SonosAppFactory<T> {
    /// `art` decodes album art away from the UI's core.
    pub fn new(client: coprocessor::sonos::Client<T>, art: album_art::Decoder) -> Self {
        let app = SonosApp::new().expect("Failed to create SonosApp");
        let info = app.global::<SonosInfo>().get_info();
        Self { transport: Some(client.0), art: Some(art), info }
    }
}

impl SonosAppFactory<SonosTransport> {
    pub fn without_client() -> Self {
        let app = SonosApp::new().expect("Failed to create SonosApp");
        let info = app.global::<SonosInfo>().get_info();
        Self { transport: None, art: None, info }
    }
}

impl Default for SonosAppFactory<SonosTransport> {
    fn default() -> Self {
        Self::without_client()
    }
}

fn format_remaining_time(duration: u32, elapsed: u32) -> String {
    let mut s = String::new();
    if duration > elapsed {
        let remaining = duration - elapsed;
        let minutes = remaining / 60;
        let seconds = remaining % 60;
        let _ = write!(s, "-{minutes:02}:{seconds:02}");
    } else {
        let _ = write!(s, "--:--");
    }
    s
}

fn format_elapsed_time(elapsed: u32) -> String {
    let mut s = String::new();
    let minutes = elapsed / 60;
    let seconds = elapsed % 60;
    let _ = write!(s, "{minutes:02}:{seconds:02}");
    s
}

#[cfg(any(test, feature = "simulator"))]
const MOCK_ALBUM_ART_JPEG: &[u8] = include_bytes!("../assets/album-art-mock.jpg");

/// Loads and prepares the default bundled mock album art for the given screen size.
fn load_default_album_art(target_size: u32) -> Option<slint::Image> {
    #[cfg(any(test, feature = "simulator"))]
    {
        album_art::decode(MOCK_ALBUM_ART_JPEG, target_size).map(slint::Image::from_rgb8)
    }
    #[cfg(not(any(test, feature = "simulator")))]
    {
        let _ = target_size;
        None
    }
}

#[cfg(any(test, feature = "simulator"))]
#[allow(dead_code)]
struct MockTrack {
    title: &'static str,
    artist: &'static str,
    album: &'static str,
    duration: u32,
}

#[cfg(any(test, feature = "simulator"))]
#[allow(dead_code)]
const MOCK_PLAYLIST: [MockTrack; 3] = [
    MockTrack {
        title: "NEVER NEVER (Extended Club Remaster)",
        artist: "Marc Moon",
        album: "Marc Moon EP",
        duration: 180,
    },
    MockTrack {
        title: "Solar Drift",
        artist: "Stellar Echo",
        album: "Cosmic Horizon",
        duration: 215,
    },
    MockTrack {
        title: "Midnight City",
        artist: "M83",
        album: "Hurry Up, We're Dreaming",
        duration: 243,
    },
];

// TODO: Load from NVS / Web dashboard once configuration UI is ready.
const SONOS_SEED_IP: Option<&str> = option_env!("SONOS_SEED_IP");

/// Album art JPEGs of unknown size get this much up front: the largest
/// measured was ~120 KB (640×640).
const ART_JPEG_RESERVE: usize = 192 * 1024;

#[derive(Default, Clone, Copy, Debug)]
struct PlaybackProgress {
    base_elapsed: u32,
    play_start: Option<core::time::Duration>,
    duration: u32,
    is_playing: bool,
}

impl PlaybackProgress {
    fn current_elapsed(&self, now: core::time::Duration) -> u32 {
        if let Some(start) = self.play_start {
            let delta = now.saturating_sub(start).as_secs() as u32;
            let el = self.base_elapsed.saturating_add(delta);
            if self.duration > 0 { el.min(self.duration) } else { el }
        } else {
            self.base_elapsed
        }
    }

    fn set_playing(&mut self, playing: bool, now: core::time::Duration) {
        if self.is_playing == playing {
            return;
        }
        if playing {
            self.play_start = Some(now);
            self.is_playing = true;
        } else {
            self.base_elapsed = self.current_elapsed(now);
            self.play_start = None;
            self.is_playing = false;
        }
    }

    fn sync_elapsed(&mut self, elapsed: u32, now: core::time::Duration) {
        self.base_elapsed = elapsed;
        if self.is_playing {
            self.play_start = Some(now);
        } else {
            self.play_start = None;
        }
    }
}

struct SonosHandles {
    _vol_debounce_timer: Rc<RefCell<slint::Timer>>,
    _hud_timer: Rc<RefCell<slint::Timer>>,
    _progress_timer: Rc<RefCell<slint::Timer>>,
    _task_handle: Option<app_shell::Handle>,
}

type BoxedCommand = Pin<Box<dyn core::future::Future<Output = ()>>>;

enum SonosUiCommand {
    Execute(BoxedCommand),
    SelectGroup(DiscoveredGroup),
    ClosePlayback,
    RefreshSummary { force: bool },
}

type SonosCommandChannel = Channel<NoopRawMutex, SonosUiCommand, 8>;

#[derive(Clone, Debug, PartialEq, Eq)]
struct DiscoveredGroup {
    id: String,
    coordinator_ip: u32,
    name: String,
}

#[derive(Debug, PartialEq, Eq)]
enum GroupReconcileAction {
    Select { index: i32, group: DiscoveredGroup, needs_playback_resubscribe: bool },
    Disappeared { fallback_index: i32, fallback_group: Option<DiscoveredGroup> },
    Empty,
}

fn reconcile_topology_groups(
    groups_map: &BTreeMap<String, (DiscoveredGroup, SonosGroupCard)>,
    active_group_id: Option<&str>,
    active_coordinator_ip: Option<u32>,
    has_active_playback: bool,
) -> (Vec<DiscoveredGroup>, Vec<SonosGroupCard>, GroupReconcileAction) {
    if groups_map.is_empty() {
        return (Vec::new(), Vec::new(), GroupReconcileAction::Empty);
    }

    let mut sorted_discovered = Vec::with_capacity(groups_map.len());
    let mut sorted_cards = Vec::with_capacity(groups_map.len());

    for (discovered, card) in groups_map.values() {
        sorted_discovered.push(discovered.clone());
        sorted_cards.push(card.clone());
    }

    let action = match (has_active_playback, active_group_id) {
        (true, Some(active_id)) => {
            if let Some(pos) = groups_map.keys().position(|k| k.as_str() == active_id) {
                let group = &groups_map[active_id].0;
                GroupReconcileAction::Select {
                    index: pos as i32,
                    group: group.clone(),
                    needs_playback_resubscribe: false,
                }
            } else if let Some((pos, (_, (group, _)))) = groups_map
                .iter()
                .enumerate()
                .find(|(_, (_, (g, _)))| Some(g.coordinator_ip) == active_coordinator_ip)
            {
                GroupReconcileAction::Select {
                    index: pos as i32,
                    group: group.clone(),
                    needs_playback_resubscribe: false,
                }
            } else {
                let fallback = sorted_discovered.first().cloned();
                GroupReconcileAction::Disappeared { fallback_index: 0, fallback_group: fallback }
            }
        }
        _ => {
            let first = &sorted_discovered[0];
            GroupReconcileAction::Select {
                index: 0,
                group: first.clone(),
                needs_playback_resubscribe: false,
            }
        }
    };

    (sorted_discovered, sorted_cards, action)
}

enum LoopEvent<TMsg, PMsg, SMsg, AMsg> {
    Topology(Option<TMsg>),
    Playback(Option<PMsg>),
    Summary(Option<SMsg>),
    AlbumArt(Option<AMsg>),
    ArtDecoded(album_art::ArtResult),
    Command(SonosUiCommand),
}

/// The decoder's next result, or never without a decoder.
async fn next_art(decoder: Option<album_art::Decoder>) -> album_art::ArtResult {
    match decoder {
        Some(decoder) => decoder.results.receive().await,
        None => core::future::pending().await,
    }
}

/// The next item of `stream`, or never if there's none open.
async fn next_item<C: tether::RawChannel, T: tether::Table>(
    stream: &mut Option<tether::Channel<C, T>>,
) -> Option<tether::Message<T, C::Buf>> {
    match stream {
        Some(stream) => stream.recv().await,
        None => core::future::pending().await,
    }
}

/// Sets up interactive handlers and timers for the Sonos controller.
///
#[allow(clippy::too_many_lines)]
fn setup_sonos<T: tether::Transport + Clone + 'static>(
    app: &SonosApp,
    context: Option<&app_shell::ShellContext>,
    client: Option<coprocessor::sonos::Client<T>>,
    art: Option<album_art::Decoder>,
) -> SonosHandles {
    // 0. Initialize album art in PSRAM sized to match screen dimensions
    let target_size = (app.get_screen_size() / 1.0).max(1.0) as u32;
    if let Some(art) = load_default_album_art(target_size) {
        app.set_album_art(art);
    }

    let command_channel = Rc::new(SonosCommandChannel::new());
    let discovered_groups = Rc::new(RefCell::new(Vec::<DiscoveredGroup>::new()));
    let transport_for_ui = client.as_ref().map(|c| c.0.clone());

    // 1. Play / Pause toggle
    let weak_play = app.as_weak();
    let cmd_play = command_channel.clone();
    let groups_play = discovered_groups.clone();
    let transport_play = transport_for_ui.clone();
    let ctx_play = context.cloned();
    app.on_toggle_play_pause(move || {
        if let Some(app) = weak_play.upgrade() {
            let mut state = app.get_state();
            let idx = state.selected_group as usize;
            state.is_playing = !state.is_playing;
            app.set_state(state);
            if let Some(ref c) = ctx_play {
                c.feedback(app_shell::Feedback::Click);
            }
            if let Some(transport) = transport_play.clone() {
                let client = coprocessor::sonos::Client(transport);
                if let Some(target) = groups_play.borrow().get(idx).cloned() {
                    let _ = cmd_play.try_send(SonosUiCommand::Execute(Box::pin(async move {
                        let mut req = coprocessor::proto::ActionRequestT::default();
                        req.endpoint_ip = target.coordinator_ip;
                        req.action = coprocessor::proto::ActionType::TogglePlayPause;
                        #[cfg(feature = "defmt")]
                        defmt::info!("Sonos: tether send:\n{}", defmt::Debug2Format(&req));
                        let _res = client.action(&req).await;
                        #[cfg(feature = "defmt")]
                        match &_res {
                            Ok(_) => defmt::info!("Sonos: Action TogglePlayPause succeeded"),
                            Err(err) => defmt::warn!(
                                "Sonos: Action TogglePlayPause failed: {}",
                                defmt::Debug2Format(err)
                            ),
                        }
                    })));
                }
            }
        }
    });

    // 2. Volume adjustment with 100ms debounce and HUD auto-dismiss
    let weak_vol = app.as_weak();
    let hud_timer = Rc::new(RefCell::new(slint::Timer::default()));
    let vol_debounce_timer = Rc::new(RefCell::new(slint::Timer::default()));
    let accumulated_delta = Rc::new(RefCell::new(0i32));

    let hud_timer_cb = hud_timer.clone();
    let vol_timer_cb = vol_debounce_timer.clone();
    let acc_cb = accumulated_delta.clone();
    let cmd_vol = command_channel.clone();
    let groups_vol = discovered_groups.clone();
    let transport_vol = transport_for_ui.clone();
    let weak_vol_timer = app.as_weak();
    let ctx_vol = context.cloned();

    app.on_adjust_volume(move |delta| {
        if let Some(app) = weak_vol.upgrade() {
            let mut state = app.get_state();
            state.volume = (state.volume + delta).clamp(0, 100);
            state.volume_visible = true;
            app.set_state(state);
            if let Some(ref c) = ctx_vol {
                c.feedback(app_shell::Feedback::DialStepForward);
            }

            *acc_cb.borrow_mut() += delta;

            let cmd = cmd_vol.clone();
            let acc = acc_cb.clone();
            let groups = groups_vol.clone();
            let transport_opt = transport_vol.clone();
            let weak_app_timer = weak_vol_timer.clone();
            vol_timer_cb.borrow_mut().start(
                slint::TimerMode::SingleShot,
                core::time::Duration::from_millis(100),
                move || {
                    let d = acc.replace(0);
                    if d != 0
                        && let Some(transport) = transport_opt.clone()
                    {
                        let client = coprocessor::sonos::Client(transport);
                        let idx = weak_app_timer
                            .upgrade()
                            .map(|a| a.get_state().selected_group as usize)
                            .unwrap_or(0);
                        if let Some(target) = groups.borrow().get(idx).cloned() {
                            let _ = cmd.try_send(SonosUiCommand::Execute(Box::pin(async move {
                                let mut req = coprocessor::proto::VolumeRequestT::default();
                                req.endpoint_ip = target.coordinator_ip;
                                req.volume = d as i16;
                                req.is_relative = true;
                                #[cfg(feature = "defmt")]
                                defmt::info!("Sonos: tether send:\n{}", defmt::Debug2Format(&req));
                                let _res = client.set_volume(&req).await;
                                #[cfg(feature = "defmt")]
                                match &_res {
                                    Ok(_) => defmt::info!("Sonos: VolumeRequest succeeded"),
                                    Err(err) => defmt::warn!(
                                        "Sonos: VolumeRequest failed: {}",
                                        defmt::Debug2Format(err)
                                    ),
                                }
                            })));
                        }
                    }
                },
            );

            let weak_hud = app.as_weak();
            hud_timer_cb.borrow_mut().start(
                slint::TimerMode::SingleShot,
                core::time::Duration::from_millis(1500),
                move || {
                    if let Some(app) = weak_hud.upgrade() {
                        let mut s = app.get_state();
                        s.volume_visible = false;
                        app.set_state(s);
                    }
                },
            );
        }
    });

    // 3. Group selection
    let weak_sel = app.as_weak();
    let cmd_sel = command_channel.clone();
    let ctx_sel = context.cloned();
    app.on_select_group(move |idx| {
        if let Some(app) = weak_sel.upgrade() {
            let mut state = app.get_state();
            let total = state.groups.row_count() as i32;
            if total > 0 {
                let clamped_idx = idx.clamp(0, total - 1);
                state.selected_group = clamped_idx;
                if let Some(group) = state.groups.row_data(clamped_idx as usize) {
                    state.active_group_name = group.name;
                }
                if state.mode == 0 {
                    let _ = cmd_sel.try_send(SonosUiCommand::RefreshSummary { force: false });
                }
            }
            app.set_state(state);
            if let Some(ref c) = ctx_sel {
                c.feedback(app_shell::Feedback::Click);
            }
        }
    });

    // 4. Mode transition (0 = Group Selector, 1 = Now Playing)
    let weak_mode = app.as_weak();
    let cmd_mode = command_channel.clone();
    let groups_mode = discovered_groups.clone();
    let ctx_mode = context.cloned();
    app.on_set_mode(move |mode| {
        if let Some(app) = weak_mode.upgrade() {
            let mut state = app.get_state();
            state.mode = mode;
            if mode == 1 {
                let idx = state.selected_group as usize;
                if let Some(target) = groups_mode.borrow().get(idx).cloned() {
                    let _ = cmd_mode.try_send(SonosUiCommand::SelectGroup(target));
                }
            } else {
                let _ = cmd_mode.try_send(SonosUiCommand::ClosePlayback);
                let _ = cmd_mode.try_send(SonosUiCommand::RefreshSummary { force: true });
            }
            app.set_state(state);
            if let Some(ref c) = ctx_mode {
                c.feedback(app_shell::Feedback::Click);
            }
        }
    });

    // 5. User interaction in Mode 0 (touch / screensaver wake / dial)
    let weak_interact = app.as_weak();
    let cmd_interact = command_channel.clone();
    app.on_user_interaction(move || {
        if let Some(app) = weak_interact.upgrade()
            && app.get_state().mode == 0
        {
            let _ = cmd_interact.try_send(SonosUiCommand::RefreshSummary { force: false });
        }
    });

    // 6. Toggle time display (remaining vs elapsed) feedback
    let ctx_time = context.cloned();
    app.on_toggle_time_display(move || {
        if let Some(ref c) = ctx_time {
            c.feedback(app_shell::Feedback::Click);
        }
    });

    // 6. Track skip callbacks
    let weak_next = app.as_weak();
    let cmd_next = command_channel.clone();
    let groups_next = discovered_groups.clone();
    let transport_next = transport_for_ui.clone();
    let ctx_next = context.cloned();
    app.on_next_track(move || {
        if let Some(app) = weak_next.upgrade() {
            if let Some(ref c) = ctx_next {
                c.feedback(app_shell::Feedback::Click);
            }
            if let Some(transport) = transport_next.clone() {
                let client = coprocessor::sonos::Client(transport);
                let idx = app.get_state().selected_group as usize;
                if let Some(target) = groups_next.borrow().get(idx).cloned() {
                    let _ = cmd_next.try_send(SonosUiCommand::Execute(Box::pin(async move {
                        let mut req = coprocessor::proto::ActionRequestT::default();
                        req.endpoint_ip = target.coordinator_ip;
                        req.action = coprocessor::proto::ActionType::Next;
                        #[cfg(feature = "defmt")]
                        defmt::info!("Sonos: tether send:\n{}", defmt::Debug2Format(&req));
                        let _res = client.action(&req).await;
                        #[cfg(feature = "defmt")]
                        match &_res {
                            Ok(_) => defmt::info!("Sonos: Action Next succeeded"),
                            Err(err) => defmt::warn!(
                                "Sonos: Action Next failed: {}",
                                defmt::Debug2Format(err)
                            ),
                        }
                    })));
                }
            }
        }
    });

    let weak_prev = app.as_weak();
    let cmd_prev = command_channel.clone();
    let groups_prev = discovered_groups.clone();
    let transport_prev = transport_for_ui.clone();
    let ctx_prev = context.cloned();
    app.on_previous_track(move || {
        if let Some(app) = weak_prev.upgrade() {
            if let Some(ref c) = ctx_prev {
                c.feedback(app_shell::Feedback::Click);
            }
            if let Some(transport) = transport_prev.clone() {
                let client = coprocessor::sonos::Client(transport);
                let idx = app.get_state().selected_group as usize;
                if let Some(target) = groups_prev.borrow().get(idx).cloned() {
                    let _ = cmd_prev.try_send(SonosUiCommand::Execute(Box::pin(async move {
                        let mut req = coprocessor::proto::ActionRequestT::default();
                        req.endpoint_ip = target.coordinator_ip;
                        req.action = coprocessor::proto::ActionType::Previous;
                        #[cfg(feature = "defmt")]
                        defmt::info!("Sonos: tether send:\n{}", defmt::Debug2Format(&req));
                        let _res = client.action(&req).await;
                        #[cfg(feature = "defmt")]
                        match &_res {
                            Ok(_) => defmt::info!("Sonos: Action Previous succeeded"),
                            Err(err) => defmt::warn!(
                                "Sonos: Action Previous failed: {}",
                                defmt::Debug2Format(err)
                            ),
                        }
                    })));
                }
            }
        }
    });

    // 7. Playback progress duration ticker
    let progress = Rc::new(RefCell::new(PlaybackProgress::default()));
    let progress_timer = Rc::new(RefCell::new(slint::Timer::default()));
    let weak_app_prog = app.as_weak();
    let progress_for_timer = progress.clone();
    let progress_context = context.cloned();
    progress_timer.borrow_mut().start(
        slint::TimerMode::Repeated,
        core::time::Duration::from_millis(500),
        move || {
            if let Some(app) = weak_app_prog.upgrade() {
                let p = progress_for_timer.borrow();
                if p.is_playing && p.duration > 0 {
                    let now = progress_context
                        .as_ref()
                        .map(|ctx| ctx.platform().duration_since_boot())
                        .unwrap_or_default();
                    let el = p.current_elapsed(now);
                    let mut state = app.get_state();
                    state.progress_ratio = (el as f32) / (p.duration as f32);
                    state.remaining_str = format_remaining_time(p.duration, el).as_str().into();
                    state.elapsed_str = format_elapsed_time(el).as_str().into();
                    app.set_state(state);
                }
            }
        },
    );

    // 8. Background Tether Task
    let Some(seed_ip) = SONOS_SEED_IP else {
        #[cfg(feature = "defmt")]
        defmt::warn!("Sonos: SONOS_SEED_IP is not set!");
        let mut state = app.get_state();
        state.status = "Seed IP missing. Set SONOS_SEED_IP at build time or via dashboard.".into();
        state.groups = slint::ModelRc::default();
        app.set_state(state);
        return SonosHandles {
            _vol_debounce_timer: vol_debounce_timer,
            _hud_timer: hud_timer,
            _progress_timer: progress_timer,
            _task_handle: None,
        };
    };

    let seed_ip_u32 = match seed_ip.parse::<core::net::Ipv4Addr>() {
        Ok(ip) => u32::from(ip),
        Err(_) => {
            #[cfg(feature = "defmt")]
            defmt::warn!("Sonos: SONOS_SEED_IP '{}' is not a valid IPv4 address!", seed_ip);
            let mut state = app.get_state();
            state.status = "Invalid seed IP. Set a valid IPv4 address for SONOS_SEED_IP.".into();
            state.groups = slint::ModelRc::default();
            app.set_state(state);
            return SonosHandles {
                _vol_debounce_timer: vol_debounce_timer,
                _hud_timer: hud_timer,
                _progress_timer: progress_timer,
                _task_handle: None,
            };
        }
    };

    let Some(client) = client else {
        let mut state = app.get_state();
        state.status = "Co-processor not linked".into();
        app.set_state(state);
        return SonosHandles {
            _vol_debounce_timer: vol_debounce_timer,
            _hud_timer: hud_timer,
            _progress_timer: progress_timer,
            _task_handle: None,
        };
    };

    let Some(context) = context else {
        return SonosHandles {
            _vol_debounce_timer: vol_debounce_timer,
            _hud_timer: hud_timer,
            _progress_timer: progress_timer,
            _task_handle: None,
        };
    };

    #[cfg(feature = "defmt")]
    defmt::info!("Sonos: SONOS_SEED_IP is set to {}", seed_ip);

    let weak_app = app.as_weak();
    let progress_task = progress.clone();

    let groups_task = discovered_groups.clone();
    let context_task = context.clone();
    let task_handle = context.schedule_future(
        "sonos",
        Box::pin(async move {
            let mut req = coprocessor::proto::TopologyRequestT::default();
            req.seed_ip = seed_ip_u32;
            #[cfg(feature = "defmt")]
            defmt::info!("Sonos: tether send:\n{}", defmt::Debug2Format(&req));
            let mut topology_stream = client.watch_topology(&req, 16);
            let mut playback_stream: Option<
                tether::Channel<T::Channel, coprocessor::proto::StateUpdate<'static>>,
            > = None;
            let mut summary_stream: Option<
                tether::Channel<T::Channel, coprocessor::proto::GroupSummary<'static>>,
            > = None;
            // Album art: the track's, downloading here, then decoded by `art`.
            // The image shown stays until the next one is ready.
            let mut art_stream: Option<
                tether::Channel<T::Channel, coprocessor::proto::AlbumArtChunk<'static>>,
            > = None;
            let mut art_uri: Option<String> = None; // Shown, or on its way.
            let mut art_generation: u32 = 0;
            let mut art_jpeg: Vec<u8> = Vec::new();
            let mut art_pending: Option<album_art::ArtRequest> = None; // Decoder busy.

            let mut current_seq: Option<u32> = None;
            let mut pending_groups: BTreeMap<String, (DiscoveredGroup, SonosGroupCard)> =
                BTreeMap::new();
            let mut active_group_id: Option<String> = None;
            let mut active_coordinator_ip: Option<u32> = None;
            let mut last_summary_at: Option<core::time::Duration> = None;

            loop {
                let event = match select(
                    select4(
                        command_channel.receive(),
                        topology_stream.recv(),
                        next_item(&mut playback_stream),
                        next_item(&mut summary_stream),
                    ),
                    select(next_item(&mut art_stream), next_art(art)),
                )
                .await
                {
                    Either::First(Either4::First(cmd)) => LoopEvent::Command(cmd),
                    Either::First(Either4::Second(msg)) => LoopEvent::Topology(msg),
                    Either::First(Either4::Third(msg)) => LoopEvent::Playback(msg),
                    Either::First(Either4::Fourth(msg)) => LoopEvent::Summary(msg),
                    Either::Second(Either::First(msg)) => LoopEvent::AlbumArt(msg),
                    Either::Second(Either::Second(result)) => LoopEvent::ArtDecoded(result),
                };

                match event {
                    LoopEvent::Topology(Some(msg)) => {
                        let update = msg.get();
                        #[cfg(feature = "defmt")]
                        defmt::info!(
                            "Sonos: received tether stream:\n{}",
                            defmt::Debug2Format(&update)
                        );
                        let seq = update.sequence();
                        let idx = update.group_index();
                        let total = update.total_groups();

                        if current_seq != Some(seq) {
                            current_seq = Some(seq);
                            pending_groups.clear();
                        }

                        if let Some(g) = update.group() {
                            let id = g.id().unwrap_or("");
                            let name = g.name().unwrap_or("");
                            let is_playing = g.is_playing();
                            let coord_ip = g.coordinator_ip();

                            let mut members_vec: Vec<slint::SharedString> = Vec::new();
                            if let Some(members) = g.members() {
                                for m in members {
                                    let m_name = m.name().unwrap_or("");
                                    members_vec.push(slint::SharedString::from(m_name));
                                }
                            }

                            let card = SonosGroupCard {
                                name: slint::SharedString::from(name),
                                members: slint::ModelRc::from(Rc::new(slint::VecModel::from(
                                    members_vec,
                                ))),
                                playing_summary: slint::SharedString::from(if is_playing {
                                    "Playing"
                                } else {
                                    "Paused"
                                }),
                                is_playing,
                            };

                            let discovered = DiscoveredGroup {
                                id: String::from(id),
                                coordinator_ip: coord_ip,
                                name: String::from(name),
                            };

                            pending_groups.insert(String::from(id), (discovered, card));
                        }

                        if total == 0 || idx + 1 >= total || pending_groups.len() >= total as usize
                        {
                            #[cfg(feature = "defmt")]
                            defmt::info!(
                                "Sonos: Topology sequence {} complete ({} groups received)",
                                seq,
                                pending_groups.len()
                            );

                            let (sorted_discovered, sorted_cards, action) =
                                reconcile_topology_groups(
                                    &pending_groups,
                                    active_group_id.as_deref(),
                                    active_coordinator_ip,
                                    playback_stream.is_some(),
                                );

                            *groups_task.borrow_mut() = sorted_discovered;

                            if let Some(app) = weak_app.upgrade() {
                                let mut state = app.get_state();
                                state.groups = slint::ModelRc::from(Rc::new(
                                    slint::VecModel::from(sorted_cards),
                                ));

                                match action {
                                    GroupReconcileAction::Select {
                                        index,
                                        group,
                                        needs_playback_resubscribe,
                                    } => {
                                        state.selected_group = index;
                                        state.active_group_name =
                                            slint::SharedString::from(group.name.as_str());
                                        active_group_id = Some(group.id.clone());
                                        active_coordinator_ip = Some(group.coordinator_ip);

                                        if needs_playback_resubscribe {
                                            let mut req =
                                                coprocessor::proto::PlaybackRequestT::default();
                                            req.endpoint_ip = group.coordinator_ip;
                                            #[cfg(feature = "defmt")]
                                            defmt::info!(
                                                "Sonos: tether send:\n{}",
                                                defmt::Debug2Format(&req)
                                            );
                                            playback_stream = Some(client.watch_playback(&req, 4));
                                        }
                                    }
                                    GroupReconcileAction::Disappeared {
                                        fallback_index,
                                        fallback_group,
                                    } => {
                                        state.mode = 0;
                                        state.selected_group = fallback_index;
                                        if let Some(group) = fallback_group {
                                            state.active_group_name =
                                                slint::SharedString::from(group.name.as_str());
                                            active_group_id = Some(group.id);
                                            active_coordinator_ip = Some(group.coordinator_ip);
                                        } else {
                                            state.active_group_name =
                                                slint::SharedString::default();
                                            active_group_id = None;
                                            active_coordinator_ip = None;
                                        }
                                        playback_stream = None;
                                        let _ = command_channel.try_send(
                                            SonosUiCommand::RefreshSummary { force: true },
                                        );
                                    }
                                    GroupReconcileAction::Empty => {
                                        state.mode = 0;
                                        state.selected_group = 0;
                                        state.active_group_name = slint::SharedString::default();
                                        active_group_id = None;
                                        active_coordinator_ip = None;
                                        playback_stream = None;
                                    }
                                }

                                if playback_stream.is_none() {
                                    let _ = command_channel
                                        .try_send(SonosUiCommand::RefreshSummary { force: true });
                                }

                                app.set_state(state);
                                context_task.platform().wake_event_loop();
                            }
                        }
                    }
                    LoopEvent::Topology(None) => {
                        let err_status = topology_stream.end();
                        #[cfg(feature = "defmt")]
                        defmt::warn!(
                            "Sonos: Topology stream ended: {}",
                            defmt::Debug2Format(&err_status)
                        );

                        if let Some(app) = weak_app.upgrade() {
                            let mut state = app.get_state();
                            state.groups = slint::ModelRc::default();
                            state.status = match err_status {
                                Some(Err(tether::Status::Unavailable)) => {
                                    "Speaker unreachable. Check seed IP or network.".into()
                                }
                                Some(Err(err)) => {
                                    alloc::format!("Connection error: {:?}", err).into()
                                }
                                _ => "Topology stream ended.".into(),
                            };
                            app.set_state(state);
                            context_task.platform().wake_event_loop();
                        }
                        break;
                    }
                    LoopEvent::Playback(Some(msg)) => {
                        let update = msg.get();
                        #[cfg(feature = "defmt")]
                        defmt::info!(
                            "Sonos: received tether stream:\n{}",
                            defmt::Debug2Format(&update)
                        );
                        // A new track's art: replacing the stream cancels one
                        // still downloading.
                        if let Some(uri) = update
                            .track()
                            .and_then(|t| t.album_art_uri())
                            .filter(|uri| !uri.is_empty())
                            && art_uri.as_deref() != Some(uri)
                            && art.is_some()
                            && let Some(endpoint) = active_coordinator_ip
                        {
                            art_uri = Some(String::from(uri));
                            art_generation = art_generation.wrapping_add(1);
                            art_jpeg = Vec::new();
                            let mut req = coprocessor::proto::AlbumArtRequestT::default();
                            req.endpoint_ip = endpoint;
                            req.uri = Some(String::from(uri));
                            art_stream = Some(client.get_album_art(&req, 8));
                        }
                        let now = context_task.platform().duration_since_boot();
                        if let Some(app) = weak_app.upgrade() {
                            let mut state = app.get_state();
                            if let Some(track) = update.track() {
                                let mut track_changed = false;
                                if let Some(title) = track.title()
                                    && !title.is_empty()
                                {
                                    if state.track_title.as_str() != title {
                                        track_changed = true;
                                    }
                                    state.track_title = slint::SharedString::from(title);
                                }
                                if let Some(artist) = track.artist()
                                    && !artist.is_empty()
                                {
                                    state.track_artist = slint::SharedString::from(artist);
                                }
                                if let Some(album) = track.album()
                                    && !album.is_empty()
                                {
                                    state.track_album = slint::SharedString::from(album);
                                }
                                if let Some(dur) = track.duration_seconds() {
                                    progress_task.borrow_mut().duration = dur;
                                }
                                if let Some(el) = track.elapsed_seconds() {
                                    progress_task.borrow_mut().sync_elapsed(el, now);
                                } else if track_changed {
                                    progress_task.borrow_mut().sync_elapsed(0, now);
                                }
                            }
                            if let Some(vol) = update.volume()
                                && !state.volume_visible
                            {
                                state.volume = vol as i32;
                            }
                            if let Some(st) = update.transport_state() {
                                match st {
                                    coprocessor::proto::TransportState::Playing => {
                                        state.is_playing = true;
                                        progress_task.borrow_mut().set_playing(true, now);
                                    }
                                    coprocessor::proto::TransportState::Stopped
                                    | coprocessor::proto::TransportState::Paused => {
                                        state.is_playing = false;
                                        progress_task.borrow_mut().set_playing(false, now);
                                    }
                                    coprocessor::proto::TransportState::Transitioning => {
                                        // Retain existing state.is_playing during track skip (matches ../review)
                                    }
                                    _ => {}
                                }
                            }

                            if let Some(ref id) = active_group_id
                                && let Some((_, card)) = pending_groups.get_mut(id.as_str())
                            {
                                if let Some(st) = update.transport_state() {
                                    card.is_playing =
                                        matches!(st, coprocessor::proto::TransportState::Playing);
                                }
                                if let Some(track) = update.track()
                                    && let Some(title) = track.title()
                                    && !title.is_empty()
                                {
                                    card.playing_summary = slint::SharedString::from(title);
                                }
                            }

                            let p = progress_task.borrow();
                            let el = p.current_elapsed(now);
                            if p.duration > 0 {
                                state.progress_ratio = (el as f32) / (p.duration as f32);
                                state.remaining_str =
                                    format_remaining_time(p.duration, el).as_str().into();
                                state.elapsed_str = format_elapsed_time(el).as_str().into();
                            } else {
                                state.progress_ratio = 0.0;
                                state.remaining_str = "--:--".into();
                                state.elapsed_str = format_elapsed_time(el).as_str().into();
                            }

                            app.set_state(state);
                            context_task.platform().wake_event_loop();
                        }
                    }
                    LoopEvent::Summary(Some(msg)) => {
                        let summary = msg.get();
                        let title = summary.title().unwrap_or("");
                        // The model lists pending_groups in order: the same index.
                        if let Some((index, (_, card))) = pending_groups
                            .values_mut()
                            .enumerate()
                            .find(|(_, (d, _))| d.coordinator_ip == summary.coordinator_ip())
                        {
                            card.is_playing = summary.is_playing();
                            card.playing_summary = if !title.is_empty() {
                                slint::SharedString::from(title)
                            } else if card.is_playing {
                                slint::SharedString::from("Playing")
                            } else {
                                slint::SharedString::from("Paused")
                            };
                            if let Some(app) = weak_app.upgrade() {
                                // Just this card: the groups themselves didn't change.
                                app.get_state().groups.set_row_data(index, card.clone());
                                context_task.platform().wake_event_loop();
                            }
                        }
                    }
                    LoopEvent::AlbumArt(Some(msg)) => {
                        let chunk = msg.get();
                        if art_jpeg.capacity() == 0 {
                            // Speakers send the art chunked, without a size:
                            // reserve what covers it (PSRAM, freed once decoded)
                            // rather than grow as it comes.
                            art_jpeg.reserve_exact(match chunk.total_size() {
                                0 => ART_JPEG_RESERVE,
                                size => size as usize,
                            });
                        }
                        if let Some(data) = chunk.data() {
                            art_jpeg.extend_from_slice(data.bytes());
                        }
                    }
                    LoopEvent::AlbumArt(None) => {
                        let status = art_stream.as_ref().and_then(|s| s.end());
                        art_stream = None; // Downloaded, or failed: closed either way.
                        let jpeg = core::mem::take(&mut art_jpeg);
                        if let Some(Ok(())) = status
                            && let Some(decoder) = art
                        {
                            let request = album_art::ArtRequest {
                                generation: art_generation,
                                jpeg,
                                size: target_size,
                            };
                            // One at a time: if the decoder is busy, this
                            // waits for its result, replacing an older one.
                            if let Err(embassy_sync::channel::TrySendError::Full(request)) =
                                decoder.requests.try_send(request)
                            {
                                art_pending = Some(request);
                            }
                        } else {
                            #[cfg(feature = "defmt")]
                            defmt::warn!(
                                "Sonos: album art failed: {}",
                                defmt::Debug2Format(&status)
                            );
                        }
                    }
                    LoopEvent::ArtDecoded(result) => {
                        if let Some(request) = art_pending.take()
                            && let Some(decoder) = art
                        {
                            let _ = decoder.requests.try_send(request);
                        }
                        if result.generation == art_generation
                            && let Some(pixels) = result.pixels
                            && let Some(app) = weak_app.upgrade()
                        {
                            app.set_album_art(slint::Image::from_rgb8(pixels));
                            context_task.platform().wake_event_loop();
                        }
                    }
                    LoopEvent::Summary(None) => {
                        let _status = summary_stream.as_ref().and_then(|s| s.end());
                        #[cfg(feature = "defmt")]
                        if !matches!(_status, Some(Ok(()))) {
                            defmt::warn!(
                                "Sonos: groups summary ended: {}",
                                defmt::Debug2Format(&_status)
                            );
                        }
                        summary_stream = None;
                    }
                    LoopEvent::Playback(None) => {
                        let _err_status = playback_stream.as_ref().and_then(|s| s.end());
                        #[cfg(feature = "defmt")]
                        defmt::warn!(
                            "Sonos: Playback stream ended: {}",
                            defmt::Debug2Format(&_err_status)
                        );
                        playback_stream = None;
                    }
                    LoopEvent::Command(cmd) => match cmd {
                        SonosUiCommand::Execute(fut) => {
                            fut.await;
                        }
                        SonosUiCommand::SelectGroup(target) => {
                            active_group_id = Some(target.id.clone());
                            active_coordinator_ip = Some(target.coordinator_ip);
                            // The new group's first update brings its art; the
                            // previous image stays meanwhile.
                            art_uri = None;
                            art_stream = None;
                            art_generation = art_generation.wrapping_add(1);
                            // Nothing of the previous group's playback: the new watch's
                            // first update carries the whole state, and a speaker with
                            // nothing to play sends no track.
                            *progress_task.borrow_mut() = PlaybackProgress::default();
                            if let Some(app) = weak_app.upgrade() {
                                let mut state = app.get_state();
                                state.track_title = "".into();
                                state.track_artist = "".into();
                                state.track_album = "".into();
                                state.is_playing = false;
                                state.progress_ratio = 0.0;
                                state.elapsed_str = "00:00".into();
                                state.remaining_str = "--:--".into();
                                app.set_state(state);
                                context_task.platform().wake_event_loop();
                            }
                            let mut req = coprocessor::proto::PlaybackRequestT::default();
                            req.endpoint_ip = target.coordinator_ip;
                            #[cfg(feature = "defmt")]
                            defmt::info!("Sonos: tether send:\n{}", defmt::Debug2Format(&req));
                            playback_stream = Some(client.watch_playback(&req, 4));
                        }
                        SonosUiCommand::ClosePlayback => {
                            playback_stream = None;
                        }
                        SonosUiCommand::RefreshSummary { force } => {
                            let now = context_task.platform().duration_since_boot();
                            let should_fetch = force
                                || match last_summary_at {
                                    None => true,
                                    Some(last) => {
                                        now.saturating_sub(last)
                                            >= core::time::Duration::from_secs(15)
                                    }
                                };
                            if should_fetch {
                                last_summary_at = Some(now);
                                #[cfg(feature = "defmt")]
                                defmt::info!("Sonos: fetching groups summary (force={})", force);
                                let coordinator_ips: Vec<u32> = pending_groups
                                    .values()
                                    .map(|(d, _)| d.coordinator_ip)
                                    .collect();
                                let mut req = coprocessor::proto::GroupsSummaryRequestT::default();
                                req.coordinator_ips = Some(coordinator_ips);
                                // Each group shows as its summary comes.
                                summary_stream = Some(client.get_groups_summary(&req, 4));
                            }
                        }
                    },
                }
            }
        }),
    );

    SonosHandles {
        _vol_debounce_timer: vol_debounce_timer,
        _hud_timer: hud_timer,
        _progress_timer: progress_timer,
        _task_handle: Some(task_handle),
    }
}

impl<T: tether::Transport + Clone + 'static> app_shell::AppFactory for SonosAppFactory<T> {
    fn info(&self) -> theme::AppInfo {
        self.info.clone()
    }

    fn launch(&self, context: app_shell::ShellContext) -> Box<dyn Any> {
        let app = SonosApp::new().expect("Failed to create SonosApp");
        let client = self.transport.clone().map(coprocessor::sonos::Client);
        let handles = setup_sonos(&app, Some(&context), client, self.art);
        context.connect(&app);

        // In Mode 1, edge back and edge quit return to Mode 0 (Groups)
        let nav = theme::Navigation::get(&app);
        let weak_nav_back = app.as_weak();
        nav.on_back(move || {
            if let Some(app) = weak_nav_back.upgrade()
                && app.get_state().mode == 1
            {
                app.invoke_set_mode(0);
                return true;
            }
            false
        });
        let weak_nav_quit = app.as_weak();
        nav.on_quit(move || {
            if let Some(app) = weak_nav_quit.upgrade()
                && app.get_state().mode == 1
            {
                app.invoke_set_mode(0);
                return true;
            }
            false
        });

        app.on_exit(move || context.exit());
        let _ = app.show();

        Box::new((app, handles))
    }

    fn clone_box(&self) -> Box<dyn app_shell::AppFactory> {
        Box::new(self.clone())
    }
}

#[cfg(test)]
mod tests {
    extern crate std;

    use core::future::Future;

    use super::*;

    #[test]
    fn test_format_remaining_time() {
        assert_eq!(format_remaining_time(180, 45), "-02:15");
        assert_eq!(format_remaining_time(180, 0), "-03:00");
        assert_eq!(format_remaining_time(60, 59), "-00:01");
        assert_eq!(format_remaining_time(60, 60), "--:--");
        assert_eq!(format_remaining_time(60, 70), "--:--");
    }

    #[test]
    fn test_format_elapsed_time() {
        assert_eq!(format_elapsed_time(0), "00:00");
        assert_eq!(format_elapsed_time(45), "00:45");
        assert_eq!(format_elapsed_time(65), "01:05");
        assert_eq!(format_elapsed_time(3600), "60:00");
    }

    #[test]
    fn test_load_default_album_art_multi_device() {
        let art_360 = load_default_album_art(360);
        assert!(art_360.is_some());
        let img_360 = art_360.unwrap();
        assert_eq!(img_360.size().width, 360);
        assert_eq!(img_360.size().height, 360);

        let art_390 = load_default_album_art(390);
        assert!(art_390.is_some());
        let img_390 = art_390.unwrap();
        assert_eq!(img_390.size().width, 390);
        assert_eq!(img_390.size().height, 390);
    }

    #[test]
    fn test_sonos_factory_without_client() {
        if std::env::var("SLINT_BACKEND").is_err() {
            unsafe {
                std::env::set_var("SLINT_BACKEND", "headless");
            }
        }
        let factory = SonosAppFactory::without_client();
        assert_eq!(app_shell::AppFactory::info(&factory).name, "Sonos");
    }

    const fn ip(a: u8, b: u8, c: u8, d: u8) -> u32 {
        ((a as u32) << 24) | ((b as u32) << 16) | ((c as u32) << 8) | (d as u32)
    }

    #[test]
    fn test_command_channel_fifo_order_and_capacity() {
        let channel = SonosCommandChannel::new();

        // 1. Fill channel to capacity (8 items)
        for i in 0..8 {
            assert!(
                channel
                    .try_send(SonosUiCommand::SelectGroup(DiscoveredGroup {
                        id: alloc::format!("RINCON_{}:1", i),
                        coordinator_ip: ip(192, 168, 1, i as u8),
                        name: alloc::format!("Room {}", i),
                    }))
                    .is_ok()
            );
        }

        // 2. 9th item must fail with Full error
        let overflow_res = channel.try_send(SonosUiCommand::SelectGroup(DiscoveredGroup {
            id: String::from("RINCON_OVERFLOW:1"),
            coordinator_ip: ip(192, 168, 1, 99),
            name: String::from("Overflow Room"),
        }));
        assert!(matches!(overflow_res, Err(embassy_sync::channel::TrySendError::Full(_))));

        // 3. Dequeue all in FIFO order
        for i in 0..8 {
            match channel.try_receive() {
                Ok(SonosUiCommand::SelectGroup(group)) => {
                    assert_eq!(group.name, alloc::format!("Room {}", i));
                    assert_eq!(group.coordinator_ip, ip(192, 168, 1, i as u8));
                }
                _ => panic!("Expected SelectGroup at index {}", i),
            }
        }

        // 4. Channel is now empty
        assert!(channel.try_receive().is_err());
    }

    #[test]
    fn test_boxed_command_execution() {
        let channel = SonosCommandChannel::new();
        let log = Rc::new(RefCell::new(Vec::<u32>::new()));

        for i in 1..=3 {
            let log_clone = log.clone();
            assert!(
                channel
                    .try_send(SonosUiCommand::Execute(Box::pin(async move {
                        log_clone.borrow_mut().push(i);
                    })))
                    .is_ok()
            );
        }

        // Poll each future to completion simulating the event loop
        let waker = core::task::Waker::noop();
        let mut cx = core::task::Context::from_waker(waker);
        while let Ok(cmd) = channel.try_receive() {
            if let SonosUiCommand::Execute(mut fut) = cmd {
                assert_eq!(fut.as_mut().poll(&mut cx), core::task::Poll::Ready(()));
            }
        }

        assert_eq!(*log.borrow(), &[1, 2, 3]);
    }

    #[test]
    fn test_command_channel_select_cancel_safety() {
        let channel = SonosCommandChannel::new();
        let executed = Rc::new(RefCell::new(false));
        let executed_clone = executed.clone();

        assert!(
            channel
                .try_send(SonosUiCommand::Execute(Box::pin(async move {
                    *executed_clone.borrow_mut() = true;
                })))
                .is_ok()
        );

        // Select between channel receive and an immediately ready future
        let res = futures_lite_select_simulation(&channel);
        assert_eq!(res, Ok(()));

        // Since the other future resolved first, channel must still hold the item un-executed
        assert!(!*executed.borrow());
        assert!(channel.try_receive().is_ok());
    }

    fn futures_lite_select_simulation(channel: &SonosCommandChannel) -> Result<(), ()> {
        // Poll select where alternative branch completes immediately
        let waker = core::task::Waker::noop();
        let mut cx = core::task::Context::from_waker(waker);
        let mut fut = embassy_futures::select::select(core::future::ready(()), channel.receive());
        match Pin::new(&mut fut).poll(&mut cx) {
            core::task::Poll::Ready(embassy_futures::select::Either::First(())) => Ok(()),
            _ => Err(()),
        }
    }

    #[test]
    fn test_playback_progress_monotonic_tracking() {
        use core::time::Duration;
        let mut progress = PlaybackProgress { duration: 200, ..Default::default() };

        // 1. Initial state: paused at 0
        assert_eq!(progress.current_elapsed(Duration::from_secs(10)), 0);

        // 2. Start playing at t = 10s
        progress.set_playing(true, Duration::from_secs(10));
        assert_eq!(progress.current_elapsed(Duration::from_secs(10)), 0);
        assert_eq!(progress.current_elapsed(Duration::from_secs(15)), 5);
        assert_eq!(progress.current_elapsed(Duration::from_secs(25)), 15);

        // 3. Pause at t = 30s (elapsed should freeze at 20s)
        progress.set_playing(false, Duration::from_secs(30));
        assert_eq!(progress.current_elapsed(Duration::from_secs(30)), 20);
        assert_eq!(progress.current_elapsed(Duration::from_secs(45)), 20);

        // 4. Sync from speaker (e.g. true paused position 22s)
        progress.sync_elapsed(22, Duration::from_secs(50));
        assert_eq!(progress.current_elapsed(Duration::from_secs(55)), 22);

        // 5. Resume playing at t = 60s
        progress.set_playing(true, Duration::from_secs(60));
        assert_eq!(progress.current_elapsed(Duration::from_secs(60)), 22);
        assert_eq!(progress.current_elapsed(Duration::from_secs(65)), 27);

        // 6. Track duration clamp: duration = 200s, advancing to 300s clamps to 200s
        assert_eq!(progress.current_elapsed(Duration::from_secs(300)), 200);
    }

    #[test]
    fn test_topology_btreemap_deterministic_ordering() {
        let mut map = BTreeMap::new();
        map.insert(
            String::from("RINCON_Z:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_Z:1"),
                    coordinator_ip: ip(192, 168, 1, 30),
                    name: String::from("Office"),
                },
                SonosGroupCard {
                    name: "Office".into(),
                    members: slint::ModelRc::default(),
                    playing_summary: "Paused".into(),
                    is_playing: false,
                },
            ),
        );
        map.insert(
            String::from("RINCON_A:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_A:1"),
                    coordinator_ip: ip(192, 168, 1, 10),
                    name: String::from("Bedroom"),
                },
                SonosGroupCard {
                    name: "Bedroom".into(),
                    members: slint::ModelRc::default(),
                    playing_summary: "Paused".into(),
                    is_playing: false,
                },
            ),
        );
        map.insert(
            String::from("RINCON_M:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_M:1"),
                    coordinator_ip: ip(192, 168, 1, 20),
                    name: String::from("Living Room"),
                },
                SonosGroupCard {
                    name: "Living Room".into(),
                    members: slint::ModelRc::default(),
                    playing_summary: "Paused".into(),
                    is_playing: false,
                },
            ),
        );

        let (sorted_discovered, sorted_cards, action) =
            reconcile_topology_groups(&map, None, None, false);

        assert_eq!(sorted_discovered.len(), 3);
        assert_eq!(sorted_cards.len(), 3);
        assert_eq!(sorted_discovered[0].id, "RINCON_A:1");
        assert_eq!(sorted_discovered[1].id, "RINCON_M:1");
        assert_eq!(sorted_discovered[2].id, "RINCON_Z:1");

        assert_eq!(
            action,
            GroupReconcileAction::Select {
                index: 0,
                group: sorted_discovered[0].clone(),
                needs_playback_resubscribe: false,
            }
        );
    }

    #[test]
    fn test_topology_btreemap_preserves_selection() {
        let mut map = BTreeMap::new();
        map.insert(
            String::from("RINCON_A:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_A:1"),
                    coordinator_ip: ip(192, 168, 1, 10),
                    name: String::from("Bedroom"),
                },
                SonosGroupCard::default(),
            ),
        );
        map.insert(
            String::from("RINCON_M:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_M:1"),
                    coordinator_ip: ip(192, 168, 1, 20),
                    name: String::from("Living Room"),
                },
                SonosGroupCard::default(),
            ),
        );
        map.insert(
            String::from("RINCON_Z:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_Z:1"),
                    coordinator_ip: ip(192, 168, 1, 30),
                    name: String::from("Office"),
                },
                SonosGroupCard::default(),
            ),
        );

        let (_, _, action) =
            reconcile_topology_groups(&map, Some("RINCON_Z:1"), Some(ip(192, 168, 1, 30)), true);

        assert_eq!(
            action,
            GroupReconcileAction::Select {
                index: 2,
                group: DiscoveredGroup {
                    id: String::from("RINCON_Z:1"),
                    coordinator_ip: ip(192, 168, 1, 30),
                    name: String::from("Office"),
                },
                needs_playback_resubscribe: false,
            }
        );
    }

    #[test]
    fn test_topology_btreemap_coordinator_match_on_regroup() {
        let mut map = BTreeMap::new();
        map.insert(
            String::from("RINCON_A:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_A:1"),
                    coordinator_ip: ip(192, 168, 1, 10),
                    name: String::from("Bedroom"),
                },
                SonosGroupCard::default(),
            ),
        );
        map.insert(
            String::from("RINCON_Z:2"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_Z:2"),
                    coordinator_ip: ip(192, 168, 1, 30),
                    name: String::from("Office + Kitchen"),
                },
                SonosGroupCard::default(),
            ),
        );

        let (_, _, action) =
            reconcile_topology_groups(&map, Some("RINCON_Z:1"), Some(ip(192, 168, 1, 30)), true);

        assert_eq!(
            action,
            GroupReconcileAction::Select {
                index: 1,
                group: DiscoveredGroup {
                    id: String::from("RINCON_Z:2"),
                    coordinator_ip: ip(192, 168, 1, 30),
                    name: String::from("Office + Kitchen"),
                },
                needs_playback_resubscribe: false,
            }
        );
    }

    #[test]
    fn test_topology_btreemap_disappeared_reverts_to_main_page() {
        let mut map = BTreeMap::new();
        map.insert(
            String::from("RINCON_A:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_A:1"),
                    coordinator_ip: ip(192, 168, 1, 10),
                    name: String::from("Bedroom"),
                },
                SonosGroupCard::default(),
            ),
        );

        let (_, _, action) =
            reconcile_topology_groups(&map, Some("RINCON_P:1"), Some(ip(192, 168, 1, 50)), true);

        assert_eq!(
            action,
            GroupReconcileAction::Disappeared {
                fallback_index: 0,
                fallback_group: Some(DiscoveredGroup {
                    id: String::from("RINCON_A:1"),
                    coordinator_ip: ip(192, 168, 1, 10),
                    name: String::from("Bedroom"),
                }),
            }
        );
    }

    #[test]
    fn test_cold_start_never_resubscribes_playback() {
        let mut map = BTreeMap::new();
        map.insert(
            String::from("RINCON_001:1"),
            (
                DiscoveredGroup {
                    id: String::from("RINCON_001:1"),
                    coordinator_ip: ip(192, 168, 9, 100),
                    name: String::from("Living Room"),
                },
                SonosGroupCard::default(),
            ),
        );

        // When has_active_playback is false, needs_playback_resubscribe MUST be false
        let (_, _, action) = reconcile_topology_groups(&map, None, None, false);
        match action {
            GroupReconcileAction::Select { needs_playback_resubscribe, .. } => {
                assert!(!needs_playback_resubscribe);
            }
            _ => panic!("Expected GroupReconcileAction::Select"),
        }
    }

    #[test]
    fn test_command_channel_handles_close_and_refresh_summary() {
        let channel = SonosCommandChannel::new();
        assert!(channel.try_send(SonosUiCommand::ClosePlayback).is_ok());
        assert!(channel.try_send(SonosUiCommand::RefreshSummary { force: true }).is_ok());
        assert!(channel.try_send(SonosUiCommand::RefreshSummary { force: false }).is_ok());

        assert!(matches!(channel.try_receive(), Ok(SonosUiCommand::ClosePlayback)));
        assert!(matches!(
            channel.try_receive(),
            Ok(SonosUiCommand::RefreshSummary { force: true })
        ));
        assert!(matches!(
            channel.try_receive(),
            Ok(SonosUiCommand::RefreshSummary { force: false })
        ));
    }
}
