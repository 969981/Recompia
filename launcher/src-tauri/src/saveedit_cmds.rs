// SPDX-License-Identifier: GPL-3.0-or-later
//! Save editor commands (saveedit.rs holds the logic). Both need the game closed: a running game holds the save open and would write
//! over an edit at its next commit, and reading while it commits could give the editor half a save.

use tauri::{AppHandle, Emitter};

use crate::config::game_dir;
use crate::guard::{ensure_idle, Need};
use crate::{saveedit, saveedit_data};

/// Whether the editor's game data (saveedit_data.rs) is made and current.
#[tauri::command]
pub fn save_edit_data_ready(app: AppHandle) -> bool {
    saveedit_data::is_ready(&game_dir(&app))
}

/// Makes the editor's game data from the game files, on a worker thread, with "save-edit-data-progress" events.
#[tauri::command]
pub async fn save_edit_prepare(app: AppHandle) -> Result<(), String> {
    ensure_idle(&app, &[Need::Update])?;
    let dir = game_dir(&app);
    tauri::async_runtime::spawn_blocking(move || {
        saveedit_data::prepare(&dir, |p| {
            let _ = app.emit("save-edit-data-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn save_edit_read(app: AppHandle) -> Result<Vec<saveedit::SaveFile>, String> {
    ensure_idle(&app, &[Need::Game("The game is running. Close it first, then edit the save.")])?;
    let dir = game_dir(&app);
    tauri::async_runtime::spawn_blocking(move || saveedit::read(&dir)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn save_edit_write(app: AppHandle, files: Vec<saveedit::SaveFile>) -> Result<saveedit::Written, String> {
    ensure_idle(&app, &[Need::Game("The game is running. Close it first, then edit the save."), Need::Update])?;
    let dir = game_dir(&app);
    tauri::async_runtime::spawn_blocking(move || saveedit::write(&dir, &files)).await.map_err(|e| e.to_string())?
}
