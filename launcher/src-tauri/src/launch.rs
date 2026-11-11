// SPDX-License-Identifier: GPL-3.0-or-later
//! Play and the game's state.

use tauri::AppHandle;

use crate::states::PlayArg;
use crate::config::load_config;
use crate::guard::{ensure_idle, Need};
use crate::paths::game_dir_of;
use crate::readiness::readiness;
use crate::{play, profiles, settings};

#[tauri::command]
pub fn play(app: AppHandle) -> Result<(), String> {
    ensure_idle(&app, &[Need::Update, Need::Storage])?;
    let cfg = load_config(&app);
    let probe = readiness(&cfg);
    let (Some(paths), "ready") = (probe.paths, probe.readiness.kind) else {
        return Err(probe.readiness.detail);
    };
    let dir = game_dir_of(&cfg);
    settings::ensure_play_paths(&dir, &paths.exefs, &paths.romfs, &paths.romfs_base, &paths.aoc_dir)?;
    profiles::apply_active(&dir)?;
    play::start(&app, &dir, &cfg)
}

#[tauri::command]
pub fn game_status(state: PlayArg) -> play::Status {
    state.0.lock().unwrap().clone()
}
