// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;

use app_shell::{AppShell, StdPlatform};
use app_sonos::SonosAppFactory;

fn main() -> Result<(), slint::PlatformError> {
    let sonos = SonosAppFactory::without_client();
    let spawner = StdPlatform::host_spawner();
    let shell = AppShell::new(spawner, Rc::new(StdPlatform::new()), Box::new(sonos));
    AppShell::start(&shell);
    slint::run_event_loop_until_quit()
}
