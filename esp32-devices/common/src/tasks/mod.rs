// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

pub mod album_art;
pub mod profiler;
pub mod screensaver;

pub use album_art::album_art_task;
pub use profiler::{
    PROFILER_ENABLED, ProfileScope, ProfilerConfig, ProfilerTrigger, TARGET_SCOPE, profiler_task,
    scope,
};
pub use screensaver::{InactivityStage, screensaver_task};
