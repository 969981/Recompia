// SPDX-License-Identifier: GPL-3.0-or-later
//! Profile commands (profiles.rs holds the logic, play_time.rs the play time).

use std::collections::HashMap;
use std::path::PathBuf;
use tauri::AppHandle;

use crate::config::game_dir;
use crate::guard::{ensure_idle, Need};
use crate::{play_time, profiles};

/// The profiles (names and ids) and which one plays.
#[tauri::command]
pub fn profiles_list(app: AppHandle) -> profiles::Store {
    profiles::load(&game_dir(&app))
}

/// Seconds played by each profile, by id, as the game counts them; a profile without a time yet is left out.
#[tauri::command]
pub fn profiles_play_time(app: AppHandle) -> HashMap<String, u64> {
    play_time::of_profiles(&game_dir(&app))
}

pub fn profiles_guard(app: &AppHandle) -> Result<PathBuf, String> {
    ensure_idle(app, &[Need::Game("The game is running. Close it first, then change profiles."), Need::Update])?;
    Ok(game_dir(app))
}

#[tauri::command]
pub fn profile_add(app: AppHandle, name: String) -> Result<profiles::Store, String> {
    profiles::add(&profiles_guard(&app)?, &name)
}

#[tauri::command]
pub fn profile_rename(app: AppHandle, id: String, name: String) -> Result<profiles::Store, String> {
    let dir = profiles_guard(&app)?;
    let s = profiles::rename(&dir, &id, &name)?;
    profiles::apply_active(&dir)?;
    Ok(s)
}

#[tauri::command]
pub fn profile_remove(app: AppHandle, id: String) -> Result<profiles::Store, String> {
    profiles::remove(&profiles_guard(&app)?, &id)
}

#[tauri::command]
pub fn profile_select(app: AppHandle, id: String) -> Result<profiles::Store, String> {
    profiles::select(&profiles_guard(&app)?, &id)
}
