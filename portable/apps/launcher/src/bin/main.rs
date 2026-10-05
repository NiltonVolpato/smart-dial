// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;

use app_launcher::LauncherAppFactory;
use app_shell::{AppShell, StdPlatform};

fn main() -> Result<(), slint::PlatformError> {
    let launcher = LauncherAppFactory::default_apps();
    let spawner = StdPlatform::host_spawner();
    let shell = AppShell::new(spawner, Rc::new(StdPlatform::new()), Box::new(launcher));
    AppShell::start(&shell);
    slint::run_event_loop_until_quit()
}
