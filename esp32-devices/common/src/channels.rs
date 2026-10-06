// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Inter-task communication channels, signals, and watches.

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_sync::watch::{DynReceiver, Watch};

use crate::event::{Event, InputEvent, ScreenEvent};

/// Central event queue capacity.
const QUEUE_CAPACITY: usize = 32;

/// Maximum number of concurrent subscribers to user activity notifications.
const MAX_ACTIVITY_SUBSCRIBERS: usize = 4;

/// Global system event channel feeding the central event loop.
pub static EVENTS: Channel<CriticalSectionRawMutex, Event, QUEUE_CAPACITY> = Channel::new();

/// Broadcast watch channel notifying subscribers of user interactions.
static USER_ACTIVITY_WATCH: Watch<CriticalSectionRawMutex, (), MAX_ACTIVITY_SUBSCRIBERS> =
    Watch::new();

/// Channel for rotary button events used to coordinate phantom touch suppression.
static BUTTON_EVENTS: Channel<CriticalSectionRawMutex, (bool, u64), 8> = Channel::new();

/// Dispatches a system event to the central event queue.
pub fn send_event(event: Event) {
    if EVENTS.try_send(event).is_err() {
        defmt::error!("EVENTS channel full, dropped event");
    }
}

/// Helper to dispatch a user input event directly.
pub fn send_input_event(input: InputEvent) {
    send_event(Event::Input(input));
}

/// Helper to dispatch a screen display event directly.
pub fn send_screen_event(screen: ScreenEvent) {
    send_event(Event::Screen(screen));
}

/// Sends an event to wake the event loop and render dirty regions.
pub fn wake_event_loop() {
    // We only need to wake up if the queue is empty, as any pending
    // events will wake up the event loop anyway.
    if EVENTS.is_empty() {
        EVENTS.try_send(Event::WakeUp).ok();
    }
}

/// Attempts to receive a pending event from the queue without blocking.
pub fn try_receive_event() -> Option<Event> {
    EVENTS.try_receive().ok()
}

/// Awaits the next event from the queue asynchronously.
pub async fn receive_event() -> Event {
    EVENTS.receive().await
}

/// Notifies all background task subscribers that user input interaction occurred.
pub fn report_user_activity() {
    USER_ACTIVITY_WATCH.sender().send(());
}

/// Subscribes to user input activity notifications.
pub fn subscribe_user_activity() -> Option<DynReceiver<'static, ()>> {
    USER_ACTIVITY_WATCH.dyn_receiver()
}

/// Reports button state changes (press/release with timestamp) for phantom touch suppression.
pub fn set_button_state(down: bool, at_ms: u64) {
    if BUTTON_EVENTS.try_send((down, at_ms)).is_err() {
        defmt::error!("BUTTON_EVENTS channel full, dropped button state");
    }
}

/// Receives any queued button state update without blocking.
pub fn try_receive_button_state() -> Option<(bool, u64)> {
    BUTTON_EVENTS.try_receive().ok()
}
