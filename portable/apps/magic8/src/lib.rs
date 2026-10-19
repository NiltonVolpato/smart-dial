// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

use alloc::boxed::Box;

slint::include_modules!();

#[derive(Clone)]
pub struct Magic8AppFactory {
    info: theme::AppInfo,
}

impl Magic8AppFactory {
    pub fn new() -> Self {
        let app = Magic8App::new().expect("Failed to create Magic8App");
        let info = app.global::<Magic8Info>().get_info();
        Self { info }
    }
}

impl Default for Magic8AppFactory {
    fn default() -> Self {
        Self::new()
    }
}

impl app_shell::AppFactory for Magic8AppFactory {
    fn info(&self) -> theme::AppInfo {
        self.info.clone()
    }

    fn launch(&self, context: app_shell::ShellContext) -> Box<dyn app_shell::ActiveApp> {
        let app = Magic8App::new().expect("Failed to create Magic8App");
        context.connect(&app);
        app.on_exit(move || context.exit());
        let _ = app.show();
        Box::new(app_shell::RunningApp::new(app))
    }

    fn clone_box(&self) -> Box<dyn app_shell::AppFactory> {
        Box::new(self.clone())
    }
}
