// SPDX-License-Identifier: GPL-3.0-or-later
//! The DLSS download (dlss.rs holds the logic).

use tauri::Emitter;
use tauri::AppHandle;

use crate::config::load_config;
use crate::guard::{ensure_idle, Need};
use crate::paths::{exe_dir, game_dir_of};
use crate::dlss;

#[tauri::command]
pub fn dlss_status(app: AppHandle) -> dlss::Status {
    dlss::status(&game_dir_of(&load_config(&app)), &exe_dir().join("tools"))
}

/// Downloads NVIDIA's nvngx_dlss.dll (see dlss.rs) on a worker thread, with "dlss-progress" events.
#[tauri::command]
pub async fn fetch_dlss(app: AppHandle) -> Result<(), String> {
    ensure_idle(&app, &[Need::Update])?;
    let dir = game_dir_of(&load_config(&app));
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        dlss::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}
