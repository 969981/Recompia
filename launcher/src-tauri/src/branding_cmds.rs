// SPDX-License-Identifier: GPL-3.0-or-later
//! The logo and icon choice (branding.rs holds the logic).

use tauri::AppHandle;

use crate::branding;
use crate::config::load_config;

/// Puts the saved logo and icon choice in place (this window, the exes, the Steam art). The page calls it after saving `logo`, `icon`
/// or `channel` (the Testing icons).
#[tauri::command]
pub async fn apply_branding(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    tauri::async_runtime::spawn_blocking(move || branding::apply(&app, &cfg)).await.map_err(|e| e.to_string())?
}
