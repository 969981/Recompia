// SPDX-License-Identifier: GPL-3.0-or-later
//! Add to Steam commands (steam.rs holds the logic).

use tauri::AppHandle;

use crate::branding::Branding;
use crate::config::load_config;
use crate::steam;

/// Add to Steam (steam.rs): what is found, and whether the game and the launcher are already in Steam.
#[tauri::command]
pub async fn steam_status(app: AppHandle) -> Result<steam::Status, String> {
    let look = Branding::of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || steam::status(&look)).await.map_err(|e| e.to_string())
}

/// `target`: "game" (starts the game at once) or "launcher" (opens this window).
#[tauri::command]
pub async fn steam_add(app: AppHandle, target: String) -> Result<String, String> {
    let look = Branding::of(&load_config(&app));
    let target = steam::Target::parse(&target)?;
    tauri::async_runtime::spawn_blocking(move || steam::add(&look, target)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn steam_remove(app: AppHandle, target: String) -> Result<String, String> {
    let look = Branding::of(&load_config(&app));
    let target = steam::Target::parse(&target)?;
    tauri::async_runtime::spawn_blocking(move || steam::remove(&look, target)).await.map_err(|e| e.to_string())?
}
