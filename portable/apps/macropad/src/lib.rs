// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

use alloc::boxed::Box;

slint::include_modules!();

#[derive(Clone)]
pub struct MacropadAppFactory {
    info: theme::AppInfo,
}

impl MacropadAppFactory {
    pub fn new() -> Self {
        let app = MacropadApp::new().expect("Failed to create MacropadApp");
        let info = app.global::<MacropadInfo>().get_info();
        Self { info }
    }
}

impl Default for MacropadAppFactory {
    fn default() -> Self {
        Self::new()
    }
}

impl app_shell::AppFactory for MacropadAppFactory {
    fn info(&self) -> theme::AppInfo {
        self.info.clone()
    }

    fn launch(&self, context: app_shell::ShellContext) -> Box<dyn app_shell::ActiveApp> {
        let app = MacropadApp::new().expect("Failed to create MacropadApp");
        context.connect(&app);
        app.on_exit(move || context.exit());
        let _ = app.show();
        Box::new(app_shell::RunningApp::new(app))
    }

    fn clone_box(&self) -> Box<dyn app_shell::AppFactory> {
        Box::new(self.clone())
    }
}
