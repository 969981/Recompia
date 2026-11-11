// SPDX-License-Identifier: GPL-3.0-or-later
//! Whether frame generation's DLL is in the game folder, and its download (framegen.rs holds the logic).

use tauri::{AppHandle, Emitter};

use crate::config::game_dir;
use crate::framegen;
use crate::guard::{ensure_idle, Need};
use crate::paths::exe_dir;

#[tauri::command]
pub fn framegen_status(app: AppHandle) -> bool {
    framegen::present(&game_dir(&app))
}

/// Downloads AMD's frame generation DLL on a worker thread, with "dlss-progress" events (one download pill serves both files).
#[tauri::command]
pub async fn fetch_framegen(app: AppHandle) -> Result<(), String> {
    ensure_idle(&app, &[Need::Update])?;
    let dir = game_dir(&app);
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        framegen::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}
