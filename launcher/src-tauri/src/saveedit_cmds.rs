// SPDX-License-Identifier: GPL-3.0-or-later
//! Save editor commands (saveedit.rs holds the logic). Both need the game closed: a running game holds the save open and would write
//! over an edit at its next commit, and reading while it commits could give the editor half a save.

use tauri::AppHandle;

use crate::config::game_dir;
use crate::guard::{ensure_idle, Need};
use crate::saveedit;

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
