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

/// Asks for the game as one package file, a .nup or an .nx.
#[tauri::command]
pub async fn pick_package(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your Pokopia .nup or .nx package").add_filter("Game package", &["nup", "nx"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

/// Asks for the DLC .nsp ("Add DLC"); None when the dialog is closed.
#[tauri::command]
pub async fn pick_nsp(app: AppHandle) -> Option<String> {
    let dlg = app.dialog().file().set_title("Choose your Pokopia DLC .nsp").add_filter("NSP package", &["nsp"]);
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}
