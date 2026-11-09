// SPDX-License-Identifier: GPL-3.0-or-later
//! Profile commands (profiles.rs holds the logic).

use std::path::PathBuf;
use tauri::{AppHandle, State};

use crate::config::load_config;
use crate::guard::{self, refuse_during_update};
use crate::paths::game_dir_of;
use crate::{play, profiles};

/// The profiles (names and ids) and which one plays.
#[tauri::command]
pub fn profiles_list(app: AppHandle) -> profiles::Store {
    profiles::load(&game_dir_of(&load_config(&app)))
}

pub fn profiles_guard(app: &AppHandle, state: &State<play::PlayState>) -> Result<PathBuf, String> {
    if guard::game_running(state) {
        return Err("The game is running. Close it first, then change profiles.".into());
    }
    refuse_during_update(app)?;
    Ok(game_dir_of(&load_config(app)))
}

#[tauri::command]
pub fn profile_add(app: AppHandle, state: State<play::PlayState>, name: String) -> Result<profiles::Store, String> {
    profiles::add(&profiles_guard(&app, &state)?, &name)
}

#[tauri::command]
pub fn profile_rename(app: AppHandle, state: State<play::PlayState>, id: String, name: String) -> Result<profiles::Store, String> {
    let dir = profiles_guard(&app, &state)?;
    let s = profiles::rename(&dir, &id, &name)?;
    profiles::apply_active(&dir)?;
    Ok(s)
}

#[tauri::command]
pub fn profile_remove(app: AppHandle, state: State<play::PlayState>, id: String) -> Result<profiles::Store, String> {
    profiles::remove(&profiles_guard(&app, &state)?, &id)
}

#[tauri::command]
pub fn profile_select(app: AppHandle, state: State<play::PlayState>, id: String) -> Result<profiles::Store, String> {
    profiles::select(&profiles_guard(&app, &state)?, &id)
}
