// SPDX-License-Identifier: GPL-3.0-or-later
//! The Mods page's commands (mods.rs, mods_catalog.rs and mods_install.rs hold the logic). They read the disk, scan packages or go to
//! the network, so they run off the UI thread. Changes wait for the game, Set up and an update to be idle: the game and Set up read
//! the mods folder and the hook plan.

use std::path::PathBuf;
use tauri::AppHandle;

use crate::config::{config_path, game_dir, load_config};
use crate::guard::{ensure_idle, Need};
use crate::mods::{self, ModsView};
use crate::mods_catalog::{self, CatalogView};
use crate::mods_update::Updated;
use crate::states::States;
use crate::{mods_install, mods_update, play};

async fn off_thread<T: Send + 'static>(f: impl FnOnce() -> Result<T, String> + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| e.to_string())?
}

/// The game folder, once nothing that reads the mods is running.
fn idle_game_dir(app: &AppHandle) -> Result<PathBuf, String> {
    ensure_idle(
        app,
        &[Need::Game("The game is running. Close it first, then change your mods."), Need::Setup("Setup is running. Wait for it to finish, then change your mods."), Need::Update],
    )?;
    Ok(game_dir(app))
}

/// Where the last good mod list is kept for browsing offline: the launcher's settings folder.
fn catalog_cache(app: &AppHandle) -> PathBuf {
    config_path(app).map(|p| p.with_file_name("mods-catalog.json")).unwrap_or_default()
}

/// The installed mods (writes their hook plan too).
#[tauri::command]
pub async fn mods_list(app: AppHandle) -> Result<ModsView, String> {
    off_thread(move || mods::list(&game_dir(&app))).await
}

/// A mod's switch. `allow`: the player said yes to running its code (asked when the list says `needs_yes`).
#[tauri::command]
pub async fn mods_set_enabled(app: AppHandle, id: String, on: bool, allow: Option<bool>) -> Result<ModsView, String> {
    let dir = idle_game_dir(&app)?;
    off_thread(move || mods::set_enabled(&dir, &id, on, allow.unwrap_or(false))).await
}

/// One place earlier (delta -1) or later (+1) in the load order.
#[tauri::command]
pub async fn mods_move(app: AppHandle, id: String, delta: i32) -> Result<ModsView, String> {
    let dir = idle_game_dir(&app)?;
    off_thread(move || mods::move_by(&dir, &id, delta)).await
}

/// A mod's setting: `number` for on/off (0/1), numbers and choices (their index), `text` for text and keys.
#[tauri::command]
pub async fn mods_set_setting(app: AppHandle, id: String, key: String, number: Option<f64>, text: Option<String>) -> Result<ModsView, String> {
    let dir = idle_game_dir(&app)?;
    off_thread(move || mods::set_setting(&dir, &id, &key, number, text.as_deref())).await
}

/// Deletes a mod from the mods folder (the page asks first).
#[tauri::command]
pub async fn mods_remove(app: AppHandle, id: String) -> Result<ModsView, String> {
    let dir = idle_game_dir(&app)?;
    off_thread(move || mods::remove(&dir, &id)).await
}

/// The Browse tab: the hub's list (this session's copy unless `refresh`; the kept copy when offline), with what is installed.
#[tauri::command]
pub async fn mods_catalog(app: AppHandle, refresh: Option<bool>) -> Result<CatalogView, String> {
    off_thread(move || {
        let (catalog, offline) = mods_catalog::load(&app.mod_catalog(), &catalog_cache(&app), refresh.unwrap_or(false))?;
        Ok(mods_catalog::view(&catalog, offline, &mods::installed_versions(&game_dir(&app))))
    })
    .await
}

/// Installs (or updates) a mod of the list after the player's yes.
#[tauri::command]
pub async fn mods_install(app: AppHandle, id: String) -> Result<ModsView, String> {
    let dir = idle_game_dir(&app)?;
    off_thread(move || mods_install::install(&dir, &app.mod_catalog(), &catalog_cache(&app), &id)).await
}

/// The update by itself: when the player's preference is on, once per launcher session, installed mods get the hub's newer versions.
/// Returns what was updated (the page says it once). Quietly nothing while the game, Set up or an update runs.
#[tauri::command]
pub async fn mods_auto_update(app: AppHandle) -> Result<Vec<Updated>, String> {
    if !load_config(&app).mods_auto_update {
        return Ok(Vec::new());
    }
    let dir = match idle_game_dir(&app) {
        Ok(dir) => dir,
        Err(why) => {
            eprintln!("mods: update by itself skipped: {why}");
            return Ok(Vec::new());
        }
    };
    if !app.mods_update().claim() {
        return Ok(Vec::new());
    }
    off_thread(move || mods_update::run(&dir, &app.mod_catalog(), &catalog_cache(&app))).await
}

/// Opens the mods folder (made when missing, so the player can drop mods into it).
#[tauri::command]
pub fn open_mods_dir(app: AppHandle) -> Result<(), String> {
    let dir = mods::mods_dir(&game_dir(&app));
    std::fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    play::open_with_shell(&dir)
}
