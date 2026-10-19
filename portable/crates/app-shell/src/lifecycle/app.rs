// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Active application representation and Slint component wrapper.

/// Trait representing an active, running application instance managed by `AppShell`.
pub trait ActiveApp: 'static {
    /// Hides the application window to release graphics resources and break internal reference cycles.
    ///
    /// The default implementation is a no-op that returns `Ok(())`, suitable for windowless test mocks.
    fn hide(&self) -> Result<(), slint::PlatformError> {
        Ok(())
    }
}

/// Blanket implementation for empty tuple (useful for testing and windowless mocks).
impl ActiveApp for () {}

/// Standard typed application wrapper for Slint components, optionally holding background handles.
pub struct RunningApp<Component, Handles = ()> {
    /// The root Slint component handle.
    pub component: Component,
    /// Background handles or timers associated with the application.
    pub handles: Handles,
}

impl<Component: slint::ComponentHandle + 'static> RunningApp<Component, ()> {
    /// Creates a new `RunningApp` wrapping a Slint component handle with no background handles.
    pub fn new(component: Component) -> Self {
        Self { component, handles: () }
    }
}

impl<Component: slint::ComponentHandle + 'static, Handles: 'static> RunningApp<Component, Handles> {
    /// Creates a new `RunningApp` wrapping a Slint component handle and associated background handles.
    pub fn with_handles(component: Component, handles: Handles) -> Self {
        Self { component, handles }
    }
}

impl<Component: slint::ComponentHandle + 'static, Handles: 'static> ActiveApp
    for RunningApp<Component, Handles>
{
    fn hide(&self) -> Result<(), slint::PlatformError> {
        self.component.hide()
    }
}
