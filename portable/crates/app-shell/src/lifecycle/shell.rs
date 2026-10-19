// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Application shell lifecycle coordinator and context.

use alloc::boxed::Box;
use alloc::rc::{Rc, Weak};
use core::cell::RefCell;
use core::future::Future;
use core::pin::Pin;

use embassy_sync::signal::Signal;

use super::task::{Handle, app_task};
use super::{ActiveApp, AppFactory, Platform};

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

    /// Connects the app's `Navigation` and `System` globals: exiting exits
    /// the app, the edge gestures' threshold clicks, the system menu's
    /// actions go to the platform, and it shows the platform's system state
    /// (see `AppShell::set_system_state`) while the app runs.
    pub fn connect<T>(&self, app: &T)
    where
        T: slint::ComponentHandle + 'static,
        for<'a> theme::Navigation<'a>: slint::Global<'a, T>,
        for<'a> theme::System<'a>: slint::Global<'a, T>,
    {
        let ctx = self.clone();
        let ctx_fb = self.clone();
        theme::connect_navigation(
            app,
            move || ctx.exit(),
            move || ctx_fb.feedback(crate::feedback::Feedback::Click),
        );
        let platform = self.platform.clone();
        let system =
            theme::connect_system(app, move |action| platform.on_system_menu_action(action));
        if let Some(shell) = self.shell.upgrade() {
            let mut shell = shell.borrow_mut();
            if let Some(state) = &shell.system_state {
                system.set_state(state);
            }
            shell.active_system = Some(system);
        }
        let _ = app.show();
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
    active_app: Option<Box<dyn ActiveApp>>,
    /// The active app's `System` global, if it connected it.
    active_system: Option<theme::SystemGlobal>,
    /// The platform's latest, for the next app; None until it reports one.
    system_state: Option<theme::SystemState>,
}

impl Drop for AppShell {
    fn drop(&mut self) {
        if let Some(active) = self.active_app.take() {
            let _ = active.hide();
        }
    }
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
            active_system: None,
            system_state: None,
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
            if let Some(active) = borrow.active_app.take() {
                let _ = active.hide();
            }
            borrow.active_system = None;
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

    /// Shows `state` in the active app's system menu, and in each app
    /// launched later.
    pub fn set_system_state(shell: &Rc<RefCell<Self>>, state: &theme::SystemState) {
        let mut shell = shell.borrow_mut();
        if let Some(system) = &shell.active_system {
            system.set_state(state);
        }
        shell.system_state = Some(state.clone());
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
