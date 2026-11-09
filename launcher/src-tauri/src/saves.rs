// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the save lives, importing one from Eden or Ryujinx, and opening its folder.

use std::fs;
use std::path::{Path, PathBuf};
use tauri::AppHandle;

use crate::config::game_dir;
use crate::guard::{ensure_idle, Need};
use crate::{play, saveimport};

/// Where the runtime keeps the save and which account folder the game uses (saveimport::locate).
pub fn save_paths(app: &AppHandle) -> (PathBuf, String) {
    saveimport::locate(&game_dir(app))
}

/// Eden and Ryujinx saves of the game found on this PC, newest first.
#[tauri::command]
pub fn save_sources() -> Vec<saveimport::Source> {
    saveimport::find_sources_here()
}

/// Imports a save: `source` is a picked folder, or None for the newest one found in Eden / Ryujinx.
#[tauri::command]
pub fn import_save(app: AppHandle, source: Option<String>) -> Result<saveimport::Report, String> {
    ensure_idle(&app, &[Need::Game("The game is running. Close it first, then import the save.")])?;
    let src = match source.filter(|s| !s.trim().is_empty()) {
        Some(p) => saveimport::resolve_picked(Path::new(p.trim()))
            .ok_or_else(|| format!("{} holds no Pokopia save (it needs the G00 and S00 folders).", p.trim()))?,
        None => {
            let found = saveimport::find_sources_here();
            let first = found.first().ok_or("No Pokopia save found in Eden or Ryujinx. Use \"Pick a folder\" instead.")?;
            PathBuf::from(&first.path)
        }
    };
    let (sd, key) = save_paths(&app);
    saveimport::import(&sd, &key, &src)
}

#[tauri::command]
pub fn open_save_dir(app: AppHandle) -> Result<(), String> {
    let (sd, _) = save_paths(&app);
    fs::create_dir_all(&sd).map_err(|e| e.to_string())?;
    play::open_with_shell(&sd)
}
