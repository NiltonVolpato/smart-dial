// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use core::any::Any;

slint::include_modules!();

#[derive(Clone)]
pub struct Magic8AppFactory {
    info: app_shell::AppInfo,
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
    fn info(&self) -> app_shell::AppInfo {
        self.info.clone()
    }

    fn launch(&self, context: app_shell::ShellContext) -> Box<dyn Any> {
        let app = Magic8App::new().expect("Failed to create Magic8App");
        let ctx = context.clone();
        let ctx_fb = context.clone();
        theme::setup_navigation(
            &app,
            move || ctx.exit(),
            move || {
                ctx_fb.feedback(app_shell::Feedback::Click);
            },
        );
        app.on_exit(move || context.exit());
        let _ = app.show();
        Box::new(app)
    }

    fn clone_box(&self) -> Box<dyn app_shell::AppFactory> {
        Box::new(self.clone())
    }
}
