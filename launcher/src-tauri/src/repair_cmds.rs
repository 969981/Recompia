// SPDX-License-Identifier: GPL-3.0-or-later
//! Repair and reset commands (repair.rs holds the logic).

use std::path::{Path, PathBuf};
use tauri::{AppHandle, State};

use crate::config::{config_path, load_config};
use crate::guard::{self, refuse_during_update};
use crate::paths::game_dir_of;
use crate::setup_job::SetupState;
use crate::shaders::RunState;
use crate::{play, repair};

/// Repair and reset (repair.rs). Planning and reading the last backup change nothing; applying and undoing need the game, Setup and
/// updates to be idle. Returns the game folder and the launcher's own settings folder, the only two places a repair may touch.
pub fn repair_paths(app: &AppHandle) -> Result<(PathBuf, PathBuf), String> {
    let cfg = config_path(app)?.parent().map(Path::to_path_buf).ok_or("The launcher settings folder is not known.")?;
    Ok((game_dir_of(&load_config(app)), cfg))
}

pub fn repair_guard(app: &AppHandle, play: &State<play::PlayState>, setup: &State<SetupState>) -> Result<(PathBuf, PathBuf), String> {
    if guard::game_running(play) {
        return Err("The game is running. Close it first, then repair.".into());
    }
    refuse_during_update(app)?;
    if setup.progress.lock().unwrap().state == RunState::Running {
        return Err("Setup is running. Wait for it to finish, then repair.".into());
    }
    repair_paths(app)
}

#[tauri::command]
pub fn repair_plan(app: AppHandle, actions: Vec<String>, groups: Option<Vec<String>>) -> Result<repair::Plan, String> {
    let (game, cfg) = repair_paths(&app)?;
    repair::plan_with(&game, &cfg, &actions, groups.as_deref())
}

#[tauri::command]
pub fn repair_apply(app: AppHandle, play: State<play::PlayState>, setup: State<SetupState>, actions: Vec<String>, groups: Option<Vec<String>>) -> Result<repair::Report, String> {
    let (game, cfg) = repair_guard(&app, &play, &setup)?;
    repair::apply_with(&game, &cfg, &actions, groups.as_deref())
}

#[tauri::command]
pub fn repair_undo(app: AppHandle, play: State<play::PlayState>, setup: State<SetupState>) -> Result<repair::Report, String> {
    let (game, cfg) = repair_guard(&app, &play, &setup)?;
    repair::undo(&game, &cfg)
}

#[tauri::command]
pub fn repair_last(app: AppHandle) -> Result<Option<repair::BackupInfo>, String> {
    let (game, _) = repair_paths(&app)?;
    Ok(repair::last_backup(&game))
}
