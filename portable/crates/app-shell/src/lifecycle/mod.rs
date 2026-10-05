// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Application lifecycle and navigation coordinator.

use alloc::boxed::Box;
use core::any::Any;

pub use theme::AppInfo;

pub mod platform;
mod shell;
pub mod task;

pub use platform::Platform;
#[cfg(feature = "std")]
pub use platform::StdPlatform;
pub use shell::{AppShell, ShellContext};
pub use task::Handle;

/// Factory trait that each application implements to participate in the App Shell.
pub trait AppFactory {
    /// Returns metadata (name, icon, accent color) for the application.
    fn info(&self) -> AppInfo;

    /// Instantiates and activates the application window, passing the shell context.
    ///
    /// Returns an opaque `Box<dyn Any>` holding the component handle / resources alive.
    fn launch(&self, context: ShellContext) -> Box<dyn Any>;

    /// Clones this factory trait object.
    fn clone_box(&self) -> Box<dyn AppFactory>;
}

impl Clone for Box<dyn AppFactory> {
    fn clone(&self) -> Self {
        self.clone_box()
    }
}
