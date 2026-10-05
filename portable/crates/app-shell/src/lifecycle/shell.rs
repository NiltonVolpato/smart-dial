// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Application shell lifecycle coordinator and context.

use alloc::boxed::Box;
use alloc::rc::{Rc, Weak};
use core::any::Any;
use core::cell::RefCell;
use core::future::Future;
use core::pin::Pin;

use embassy_sync::signal::Signal;

use super::task::{Handle, app_task};
use super::{AppFactory, Platform};

/// Context provided to an active application to interact with the shell lifecycle and platform.
#[derive(Clone)]
pub struct ShellContext {
    shell: Weak<RefCell<AppShell>>,
    platform: Rc<dyn Platform>,
    spawner: embassy_executor::Spawner,
}

impl ShellContext {
    /// Creates a new `ShellContext` from weak shell reference, platform handle, and task spawner.
    pub(crate) fn new(
        shell: Weak<RefCell<AppShell>>,
        platform: Rc<dyn Platform>,
        spawner: embassy_executor::Spawner,
    ) -> Self {
        Self { shell, platform, spawner }
    }

    /// Returns a reference to the underlying platform services.
    pub fn platform(&self) -> &dyn Platform {
        &*self.platform
    }

    /// Returns the Embassy task spawner.
    pub fn spawner(&self) -> embassy_executor::Spawner {
        self.spawner
    }

    /// Signals the shell that the current application wishes to exit.
    pub fn exit(&self) {
        if let Some(shell) = self.shell.upgrade() {
            AppShell::step(shell);
        }
    }

    /// Queues the specified application to run once upon the next transition.
    pub fn set_next_app(&self, factory: Box<dyn AppFactory>) {
        if let Some(shell) = self.shell.upgrade() {
            shell.borrow_mut().set_next_app(factory);
        }
    }

    /// Emits audible or haptic feedback through the platform.
    pub fn feedback(&self, feedback: crate::feedback::Feedback) {
        self.platform.signal_feedback(feedback);
    }

    /// Schedules a background asynchronous future on the Embassy task runner.
    ///
    /// The background task continues running as long as the returned [`Handle`] is kept alive.
    /// Dropping the handle automatically cancels the task.
    pub fn schedule_future(
        &self,
        name: &'static str,
        future: Pin<Box<dyn Future<Output = ()>>>,
    ) -> Handle {
        let cancel = Rc::new(Signal::new());
        let handle = Handle(Rc::clone(&cancel));
        let token = app_task(name, future, cancel).expect("Failed to create app_task");
        self.spawner.spawn(token);
        handle
    }
}

/// Headless application lifecycle coordinator.
///
/// Manages the active application instance and handles transitions between default
/// and temporary (run-once) applications without creating any windows of its own.
pub struct AppShell {
    spawner: embassy_executor::Spawner,
    platform: Rc<dyn Platform>,
    default_app: Box<dyn AppFactory>,
    next_app: Option<Box<dyn AppFactory>>,
    active_app: Option<Box<dyn Any>>,
}

impl AppShell {
    /// Creates a new `AppShell` with the given task spawner, platform hooks, and default application factory.
    pub fn new(
        spawner: embassy_executor::Spawner,
        platform: Rc<dyn Platform>,
        default_app: Box<dyn AppFactory>,
    ) -> Rc<RefCell<Self>> {
        Rc::new(RefCell::new(Self {
            spawner,
            platform,
            default_app,
            next_app: None,
            active_app: None,
        }))
    }

    /// Starts the shell lifecycle, launching the initial application.
    pub fn start(shell: &Rc<RefCell<Self>>) {
        Self::step(shell.clone());
    }

    /// Steps the lifecycle to the next application:
    ///
    /// 1. Drops the currently active application (triggering RAII cleanup).
    /// 2. If `next_app` is set, consumes it and launches that application.
    /// 3. Otherwise, launches the `default_app`.
    pub(crate) fn step(shell: Rc<RefCell<Self>>) {
        let (next_factory, platform, spawner) = {
            let mut borrow = shell.borrow_mut();
            borrow.active_app = None;
            let factory = borrow.next_app.take().unwrap_or_else(|| borrow.default_app.clone());
            (factory, borrow.platform.clone(), borrow.spawner)
        };

        let info = next_factory.info();
        let context = ShellContext::new(Rc::downgrade(&shell), platform.clone(), spawner);

        let instance = next_factory.launch(context);
        {
            let mut borrow = shell.borrow_mut();
            borrow.active_app = Some(instance);
            platform.on_app_launched(&info);
        }
    }

    /// Configures an application to run once upon the next lifecycle transition.
    pub(crate) fn set_next_app(&mut self, factory: Box<dyn AppFactory>) {
        self.next_app = Some(factory);
    }

    /// Signals an external exit (e.g. user gesture or button long-press) for the active app.
    pub fn exit_active_app(shell: &Rc<RefCell<Self>>) {
        Self::step(shell.clone());
    }

    /// Returns `true` if an application instance is currently active.
    pub fn has_active_app(&self) -> bool {
        self.active_app.is_some()
    }
}
