// SPDX-License-Identifier: GPL-3.0-or-later
//! Add to Steam commands (steam.rs holds the logic).

use tauri::AppHandle;

use crate::branding::Branding;
use crate::config::load_config;
use crate::steam;

/// Add to Steam (steam.rs): what is found, and whether the launcher (and the old game shortcut) are already in Steam.
#[tauri::command]
pub async fn steam_status(app: AppHandle) -> Result<steam::Status, String> {
    let look = Branding::of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || steam::status(&look)).await.map_err(|e| e.to_string())
}

/// Adds the launcher shortcut (it opens this window).
#[tauri::command]
pub async fn steam_add(app: AppHandle) -> Result<String, String> {
    let look = Branding::of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || steam::add(&look)).await.map_err(|e| e.to_string())?
}

/// `target`: "launcher" (the shortcut added here) or "game" (the old one an earlier launcher added; it can only be removed).
#[tauri::command]
pub async fn steam_remove(app: AppHandle, target: String) -> Result<String, String> {
    let look = Branding::of(&load_config(&app));
    let target = steam::Target::parse(&target)?;
    tauri::async_runtime::spawn_blocking(move || steam::remove(&look, target)).await.map_err(|e| e.to_string())?
}
