// SPDX-License-Identifier: GPL-3.0-or-later
//! Reading and writing the runtime's ini settings (settings.rs holds the logic).

use tauri::AppHandle;

use crate::config::load_config;
use crate::paths::game_dir_of;
use crate::settings;

#[tauri::command]
pub fn read_settings(app: AppHandle) -> settings::SettingsDump {
    settings::read(&game_dir_of(&load_config(&app)))
}

#[tauri::command]
pub fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir_of(&load_config(&app)), &edits)
}
