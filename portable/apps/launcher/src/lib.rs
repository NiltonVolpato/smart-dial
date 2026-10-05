// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![cfg_attr(not(test), no_std)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::any::Any;
use core::cell::RefCell;

pub use app_shell::{AppFactory, AppShell, ShellContext};
use slint::ComponentHandle;
pub use theme::AppInfo;

slint::include_modules!();

/// Application factory for the launcher carousel interface.
#[derive(Clone)]
pub struct LauncherAppFactory {
    factories: Vec<Box<dyn AppFactory>>,
    selected_index: Rc<RefCell<usize>>,
}

impl LauncherAppFactory {
    /// Creates a new `LauncherAppFactory` that presents the given list of applications.
    pub fn new(factories: Vec<Box<dyn AppFactory>>) -> Self {
        Self { factories, selected_index: Rc::new(RefCell::new(0)) }
    }

    /// Creates a new `LauncherAppFactory` configured with the default suite of applications.
    #[cfg(feature = "apps")]
    pub fn default_apps() -> Self {
        Self::default_apps_with_sonos(Box::new(app_sonos::SonosAppFactory::without_client()))
    }

    /// Creates a new `LauncherAppFactory` configured with the default suite of applications,
    /// injecting a custom Sonos factory (e.g. configured with an active Tether RPC client).
    #[cfg(feature = "apps")]
    pub fn default_apps_with_sonos(sonos_factory: Box<dyn AppFactory>) -> Self {
        let factories: Vec<Box<dyn AppFactory>> = alloc::vec![
            Box::new(app_clock::ClockAppFactory::new()),
            Box::new(app_pomodoro::PomodoroAppFactory::new()),
            Box::new(app_macropad::MacropadAppFactory::new()),
            Box::new(app_simon::SimonAppFactory::new()),
            Box::new(app_magic8::Magic8AppFactory::new()),
            sonos_factory,
        ];
        Self::new(factories)
    }
}

impl AppFactory for LauncherAppFactory {
    fn info(&self) -> AppInfo {
        AppInfo {
            name: "Launcher".into(),
            accent: slint::Color::from_argb_u8(255, 255, 255, 255),
            icon: slint::Image::default(),
        }
    }

    fn launch(&self, context: ShellContext) -> Box<dyn Any> {
        let launcher = LauncherApp::new().expect("Failed to create LauncherApp");
        let selected = *self.selected_index.borrow();
        launcher.set_selected(selected as i32);

        let cards: Vec<AppInfo> = self.factories.iter().map(|f| f.info()).collect();
        launcher.set_cards(Rc::new(slint::VecModel::from(cards)).into());

        let ctx_fb = context.clone();
        let factories = self.factories.clone();
        let selected_index = self.selected_index.clone();
        launcher.on_launch(move |idx| {
            let idx = idx as usize;
            if let Some(target_factory) = factories.get(idx) {
                selected_index.replace(idx);
                context.set_next_app(target_factory.clone_box());
                context.exit();
            }
        });

        theme::setup_navigation(
            &launcher,
            || {},
            move || {
                ctx_fb.feedback(app_shell::Feedback::Click);
            },
        );

        let _ = launcher.show();
        Box::new(launcher)
    }

    fn clone_box(&self) -> Box<dyn AppFactory> {
        Box::new(self.clone())
    }
}

#[cfg(test)]
mod tests {
    use app_shell::StdPlatform;
    use slint::Model;

    use super::*;

    #[derive(Clone)]
    struct MockAppFactory(&'static str);

    impl AppFactory for MockAppFactory {
        fn info(&self) -> AppInfo {
            AppInfo {
                name: self.0.into(),
                accent: slint::Color::from_argb_u8(255, 255, 0, 0),
                icon: slint::Image::default(),
            }
        }

        fn launch(&self, _context: ShellContext) -> Box<dyn Any> {
            Box::new(())
        }

        fn clone_box(&self) -> Box<dyn AppFactory> {
            Box::new(self.clone())
        }
    }

    fn test_factories() -> Vec<Box<dyn AppFactory>> {
        alloc::vec![
            Box::new(MockAppFactory("App 1")),
            Box::new(MockAppFactory("App 2")),
            Box::new(MockAppFactory("App 3")),
        ]
    }

    #[test]
    fn test_launcher_factory() {
        if std::env::var("SLINT_BACKEND").is_err() {
            unsafe {
                std::env::set_var("SLINT_BACKEND", "headless");
            }
        }

        let launcher = LauncherApp::new().expect("Failed to create LauncherApp");
        let cards: Vec<AppInfo> = test_factories().iter().map(|f| f.info()).collect();
        launcher.set_cards(Rc::new(slint::VecModel::from(cards)).into());
        assert_eq!(launcher.get_cards().row_count(), 3);

        // Clamping at lower bound: cannot regress before 0
        launcher.set_selected(0);
        launcher.invoke_select_prev();
        assert_eq!(launcher.get_selected(), 0);

        // Stepping forward
        launcher.invoke_select_next();
        assert_eq!(launcher.get_selected(), 1);

        launcher.invoke_select_next();
        assert_eq!(launcher.get_selected(), 2);

        // Clamping at upper bound: cannot advance past 2
        launcher.invoke_select_next();
        assert_eq!(launcher.get_selected(), 2);

        // Stepping backward
        launcher.invoke_select_prev();
        assert_eq!(launcher.get_selected(), 1);

        drop(launcher);

        let launcher_factory = LauncherAppFactory::new(test_factories());
        let spawner = StdPlatform::host_spawner();
        let shell = AppShell::new(spawner, Rc::new(StdPlatform::new()), Box::new(launcher_factory));
        AppShell::start(&shell);

        assert!(shell.borrow().has_active_app());
    }
}
