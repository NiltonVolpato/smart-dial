// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use core::any::Any;

slint::include_modules!();

#[derive(Clone)]
pub struct SimonAppFactory {
    info: theme::AppInfo,
}

impl SimonAppFactory {
    pub fn new() -> Self {
        let app = SimonApp::new().expect("Failed to create SimonApp");
        let info = app.global::<SimonInfo>().get_info();
        Self { info }
    }
}

impl Default for SimonAppFactory {
    fn default() -> Self {
        Self::new()
    }
}

impl app_shell::AppFactory for SimonAppFactory {
    fn info(&self) -> theme::AppInfo {
        self.info.clone()
    }

    fn launch(&self, context: app_shell::ShellContext) -> Box<dyn Any> {
        let app = SimonApp::new().expect("Failed to create SimonApp");
        context.connect(&app);
        app.on_exit(move || context.exit());
        let _ = app.show();
        Box::new(app)
    }

    fn clone_box(&self) -> Box<dyn app_shell::AppFactory> {
        Box::new(self.clone())
    }
}
