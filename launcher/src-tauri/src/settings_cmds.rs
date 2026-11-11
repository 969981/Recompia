// SPDX-License-Identifier: GPL-3.0-or-later
//! Reading and writing the runtime's ini settings (settings.rs holds the logic).

use tauri::AppHandle;

use crate::config::game_dir;
use crate::prompts;
use crate::settings;

#[tauri::command]
pub fn read_settings(app: AppHandle) -> settings::SettingsDump {
    settings::read(&game_dir(&app))
}

#[tauri::command]
pub fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir(&app), &edits)
}

/// The button icon pack's bytes, for the Button icons preview (an ArrayBuffer on the page).
#[tauri::command]
pub fn prompt_icon_pack(app: AppHandle) -> Result<tauri::ipc::Response, String> {
    prompts::read_pack(&game_dir(&app)).map(tauri::ipc::Response::new)
}
