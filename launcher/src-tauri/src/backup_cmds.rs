// SPDX-License-Identifier: GPL-3.0-or-later
//! Save backup commands (savebackup.rs holds the logic).

use std::fs;
use tauri::AppHandle;

use crate::config::load_config;
use crate::states::States;
use crate::guard::{self, refuse_during_update};
use crate::paths::game_dir_of;
use crate::{play, savebackup};

/// Save backups (savebackup.rs): the status, the list and a copy on request work at any time; a restore needs the game closed. They run
/// on a worker thread because a copy can wait a moment for the runtime to finish writing the save.
#[tauri::command]
pub async fn backup_status(app: AppHandle) -> Result<savebackup::Status, String> {
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::status(&dir)).await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn backup_list(app: AppHandle) -> Result<Vec<savebackup::Entry>, String> {
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::list(&dir)).await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn backup_now(app: AppHandle) -> Result<savebackup::Outcome, String> {
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::snapshot(&dir, savebackup::KIND_MANUAL)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn backup_restore(app: AppHandle, name: String) -> Result<savebackup::Restored, String> {
    if guard::game_running(&app.play()) {
        return Err("The game is running. Close it first, then restore a backup.".into());
    }
    refuse_during_update(&app)?;
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::restore(&dir, &name)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub fn open_backup_dir(app: AppHandle) -> Result<(), String> {
    let dir = game_dir_of(&load_config(&app)).join(savebackup::DIR);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    play::open_with_shell(&dir)
}
