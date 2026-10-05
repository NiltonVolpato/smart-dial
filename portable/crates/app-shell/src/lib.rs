// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Headless Application Shell: lifecycle coordination and system services.

#![cfg_attr(all(not(feature = "std"), not(test)), no_std)]

extern crate alloc;

pub mod feedback;
pub mod lifecycle;
pub mod perf;
pub mod profile;

pub use feedback::Feedback;
pub use jiff::{Timestamp, Zoned};
#[cfg(feature = "std")]
pub use lifecycle::StdPlatform;
pub use lifecycle::{AppFactory, AppInfo, AppShell, Handle, Platform, ShellContext};
pub use perf::{FrameCycles, PerfSummary, PerfTracker};
pub use profile::{DEFAULT_TABLE_CAPACITY, PcSample, SampleTable};

#[cfg(test)]
mod tests {
    use alloc::boxed::Box;
    use alloc::rc::Rc;
    use alloc::string::String;
    use core::any::Any;
    use core::cell::RefCell;

    use super::*;

    #[derive(Clone)]
    struct MockAppFactory {
        name: &'static str,
        launched_count: Rc<RefCell<usize>>,
        on_launch_cb: Option<Rc<dyn Fn(ShellContext)>>,
    }

    impl MockAppFactory {
        fn new(name: &'static str) -> (Self, Rc<RefCell<usize>>) {
            let count = Rc::new(RefCell::new(0));
            (Self { name, launched_count: count.clone(), on_launch_cb: None }, count)
        }

        fn with_action(
            name: &'static str,
            action: impl Fn(ShellContext) + 'static,
        ) -> (Self, Rc<RefCell<usize>>) {
            let count = Rc::new(RefCell::new(0));
            (
                Self { name, launched_count: count.clone(), on_launch_cb: Some(Rc::new(action)) },
                count,
            )
        }
    }

    impl AppFactory for MockAppFactory {
        fn info(&self) -> AppInfo {
            AppInfo {
                name: self.name.into(),
                accent: slint::Color::from_argb_u8(255, 0, 128, 255),
                icon: slint::Image::default(),
            }
        }

        fn launch(&self, context: ShellContext) -> Box<dyn Any> {
            *self.launched_count.borrow_mut() += 1;
            if let Some(ref cb) = self.on_launch_cb {
                cb(context);
            }
            Box::new(self.name)
        }

        fn clone_box(&self) -> Box<dyn AppFactory> {
            Box::new(self.clone())
        }
    }

    struct TestPlatform {
        launched_names: Rc<RefCell<alloc::vec::Vec<String>>>,
        wake_count: Rc<RefCell<usize>>,
        feedback_events: Rc<RefCell<alloc::vec::Vec<Feedback>>>,
    }

    impl TestPlatform {
        fn new() -> Self {
            Self {
                launched_names: Rc::new(RefCell::new(alloc::vec::Vec::new())),
                wake_count: Rc::new(RefCell::new(0)),
                feedback_events: Rc::new(RefCell::new(alloc::vec::Vec::new())),
            }
        }
    }

    impl Platform for TestPlatform {
        fn on_app_launched(&self, info: &AppInfo) {
            self.launched_names.borrow_mut().push(info.name.as_str().into());
        }
        fn wake_event_loop(&self) {
            *self.wake_count.borrow_mut() += 1;
        }
        fn now_utc(&self) -> Option<Timestamp> {
            None
        }
        fn now_local(&self) -> Option<jiff::Zoned> {
            None
        }
        fn duration_since_boot(&self) -> core::time::Duration {
            core::time::Duration::from_secs(123)
        }
        fn signal_feedback(&self, feedback: Feedback) {
            self.feedback_events.borrow_mut().push(feedback);
        }
    }

    fn test_spawner() -> embassy_executor::Spawner {
        StdPlatform::host_spawner()
    }

    #[test]
    fn test_single_app_lifecycle() {
        let (clock, launch_count) = MockAppFactory::new("Clock");
        let shell = AppShell::new(test_spawner(), Rc::new(StdPlatform::new()), Box::new(clock));

        assert!(!shell.borrow().has_active_app());
        assert_eq!(*launch_count.borrow(), 0);

        // Start shell: boots standalone app
        AppShell::start(&shell);
        assert!(shell.borrow().has_active_app());
        assert_eq!(*launch_count.borrow(), 1);

        // External exit (e.g. gesture): restarts default app
        AppShell::exit_active_app(&shell);
        assert!(shell.borrow().has_active_app());
        assert_eq!(*launch_count.borrow(), 2);
    }

    #[test]
    fn test_platform_on_app_launched_hook() {
        let platform = Rc::new(TestPlatform::new());
        let launched_names = platform.launched_names.clone();
        let (clock, _) = MockAppFactory::new("Clock");
        let shell = AppShell::new(test_spawner(), platform, Box::new(clock));

        AppShell::start(&shell);
        assert_eq!(*launched_names.borrow(), alloc::vec!["Clock"]);

        AppShell::exit_active_app(&shell);
        assert_eq!(*launched_names.borrow(), alloc::vec!["Clock", "Clock"]);
    }

    #[test]
    fn test_launcher_next_app_lifecycle() {
        let (target_app, target_launch_count) = MockAppFactory::new("Simon");

        // Launcher sets next_app and exits immediately
        let target_clone = target_app.clone();
        let (launcher, launcher_launch_count) =
            MockAppFactory::with_action("Launcher", move |ctx| {
                ctx.set_next_app(Box::new(target_clone.clone()));
                ctx.exit();
            });

        let shell = AppShell::new(test_spawner(), Rc::new(StdPlatform::new()), Box::new(launcher));

        // Starting shell launches Launcher, which queues Simon and exits, so Simon runs
        AppShell::start(&shell);
        assert_eq!(*launcher_launch_count.borrow(), 1);
        assert_eq!(*target_launch_count.borrow(), 1);

        // When Simon exits, shell returns to default app (Launcher)
        AppShell::exit_active_app(&shell);
        // Launcher ran again, which once more launched Simon
        assert_eq!(*launcher_launch_count.borrow(), 2);
        assert_eq!(*target_launch_count.borrow(), 2);
    }

    #[test]
    fn test_feedback_service() {
        let platform = Rc::new(TestPlatform::new());
        let feedback_events = platform.feedback_events.clone();

        let captured_ctx = Rc::new(RefCell::new(None));
        let captured_ctx_clone = captured_ctx.clone();
        let (app, _) = MockAppFactory::with_action("App", move |ctx| {
            *captured_ctx_clone.borrow_mut() = Some(ctx);
        });

        let shell = AppShell::new(test_spawner(), platform, Box::new(app));
        AppShell::start(&shell);

        let ctx = captured_ctx.borrow().clone().expect("Context should be captured");
        ctx.feedback(Feedback::DialStepForward);
        ctx.feedback(Feedback::DialStepBackward);
        ctx.feedback(Feedback::Click);
        ctx.feedback(Feedback::Haptic);
        ctx.feedback(Feedback::Tone { hz: 440, ms: 100 });

        assert_eq!(
            *feedback_events.borrow(),
            alloc::vec![
                Feedback::DialStepForward,
                Feedback::DialStepBackward,
                Feedback::Click,
                Feedback::Haptic,
                Feedback::Tone { hz: 440, ms: 100 }
            ]
        );
    }

    #[test]
    fn test_wake_event_loop() {
        let platform = Rc::new(TestPlatform::new());
        let wake_count = platform.wake_count.clone();

        let captured_ctx = Rc::new(RefCell::new(None));
        let captured_ctx_clone = captured_ctx.clone();
        let (app, _) = MockAppFactory::with_action("App", move |ctx| {
            *captured_ctx_clone.borrow_mut() = Some(ctx);
        });

        let shell = AppShell::new(test_spawner(), platform, Box::new(app));
        AppShell::start(&shell);

        assert_eq!(*wake_count.borrow(), 0);
        let ctx = captured_ctx.borrow().clone().expect("Context should be captured");
        ctx.platform().wake_event_loop();
        assert_eq!(*wake_count.borrow(), 1);
        assert_eq!(ctx.platform().duration_since_boot(), core::time::Duration::from_secs(123));
    }

    static TASK_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn test_schedule_future_raii_cancellation() {
        let _guard = TASK_TEST_LOCK.lock().unwrap();
        let (app, _) = MockAppFactory::with_action("TaskApp", |ctx| {
            let handle = ctx.schedule_future(
                "test_task",
                Box::pin(async {
                    core::future::pending::<()>().await;
                }),
            );
            // Drop handle immediately to verify cancel
            drop(handle);
        });

        let shell = AppShell::new(test_spawner(), Rc::new(StdPlatform::new()), Box::new(app));
        AppShell::start(&shell);
    }

    #[test]
    fn test_consecutive_app_transition_task_overlap() {
        let _guard = TASK_TEST_LOCK.lock().unwrap();
        #[allow(dead_code)]
        struct AppHandles(Handle);

        let (app2, _) = MockAppFactory::with_action("App2", |ctx| {
            let handle = ctx.schedule_future(
                "app2_task",
                Box::pin(async {
                    core::future::pending::<()>().await;
                }),
            );
            let _ = AppHandles(handle);
        });

        let app2_clone = app2.clone();
        let (app1, _) = MockAppFactory::with_action("App1", move |ctx| {
            let handle = ctx.schedule_future(
                "app1_task",
                Box::pin(async {
                    core::future::pending::<()>().await;
                }),
            );
            let _ = AppHandles(handle);
            ctx.set_next_app(Box::new(app2_clone.clone()));
        });

        let shell = AppShell::new(test_spawner(), Rc::new(StdPlatform::new()), Box::new(app1));
        AppShell::start(&shell);
        // Step directly from App1 into App2 synchronously
        AppShell::step(shell.clone());
    }
}
