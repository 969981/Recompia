// SPDX-License-Identifier: GPL-3.0-or-later
//! File and folder pickers.

use std::path::PathBuf;
use tauri::AppHandle;
use tauri_plugin_dialog::DialogExt;

use crate::dump;

#[tauri::command]
pub async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
pub async fn pick_nup(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your Pokopia .nup package").add_filter("NUP package", &["nup"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}
