// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Application background task runner and RAII cancellation handle.

use alloc::boxed::Box;
use alloc::rc::Rc;
use core::future::Future;
use core::pin::Pin;

use embassy_futures::select::{Either, select};
use embassy_sync::blocking_mutex::raw::NoopRawMutex;
use embassy_sync::signal::Signal;

/// Dedicated Embassy task driving an application's background future.
///
/// Note on `pool_size = 2`:
/// During an application transition in `AppShell::step`, the outgoing application is dropped
/// synchronously, which signals cancellation on its task handle. However, because Core 0 is
/// running `AppShell::step` synchronously, the outgoing task has not yet yielded or returned to
/// Embassy's executor to be despawned. If the newly launched incoming application synchronously
/// calls `schedule_future`, the old task is guaranteed to still occupy a slot in the pool.
/// Double-buffering with `pool_size = 2` ensures the incoming application can immediately
/// allocate its task slot while the outgoing task finishes cancellation and despawns on the
/// very next executor poll.
#[embassy_executor::task(pool_size = 2)]
pub async fn app_task(
    name: &'static str,
    future: Pin<Box<dyn Future<Output = ()>>>,
    cancel: Rc<Signal<NoopRawMutex, ()>>,
) {
    defmt::debug!("app_task `{}` started", name);
    match select(future, cancel.wait()).await {
        Either::First(()) => {
            defmt::debug!("app_task `{}` finished naturally", name);
        }
        Either::Second(()) => {
            defmt::debug!("app_task `{}` cancelled", name);
        }
    }
}

/// RAII handle to an active background application task.
///
/// When dropped, the background task is automatically signalled to abort.
pub struct Handle(pub(crate) Rc<Signal<NoopRawMutex, ()>>);

impl Handle {
    /// Explicitly aborts the background task.
    pub fn cancel(&self) {
        self.0.signal(());
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.cancel();
    }
}
