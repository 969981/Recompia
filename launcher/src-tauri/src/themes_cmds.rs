// SPDX-License-Identifier: GPL-3.0-or-later
//! The player's themes (themes.rs holds the logic).

use tauri::AppHandle;

use crate::config::{load_config, store_config};
use crate::themes::{self, Added, ThemeList};

/// Every theme pack in the themes folder, with the reason for each one that cannot be used.
#[tauri::command]
pub async fn list_themes() -> Result<ThemeList, String> {
    tauri::async_runtime::spawn_blocking(themes::list).await.map_err(|e| e.to_string())
}

/// Adds a pack from a folder or a .zip (a path the player picked).
#[tauri::command]
pub async fn add_theme(path: String) -> Result<Added, String> {
    tauri::async_runtime::spawn_blocking(move || themes::add(std::path::Path::new(&path))).await.map_err(|e| e.to_string())?
}

/// Removes a pack by its folder name. The chosen theme goes back to the launcher's own when it was one of this pack's.
#[tauri::command]
pub async fn remove_theme_pack(app: AppHandle, pack: String) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        themes::remove(&pack)?;
        let mut cfg = load_config(&app);
        if cfg.theme.split('/').next() == Some(pack.as_str()) {
            cfg.theme.clear();
            store_config(&app, &cfg)?;
        }
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Opens the themes folder in the system's file manager.
#[tauri::command]
pub fn open_themes_dir() -> Result<(), String> {
    crate::play::open_with_shell(themes::ensure_dir()?)
}
