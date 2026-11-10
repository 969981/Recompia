// SPDX-License-Identifier: GPL-3.0-or-later
//! The Testing tab's commands (the work is in testlogs.rs): list and read the logs, export the redacted zip, open its folder.

use tauri::AppHandle;

use crate::config::{game_dir, load_config};
use crate::{play, testlogs};

/// The log files that exist (pkp.log, setup.log, the crash files, pkp.ini), for the viewer's list.
#[tauri::command]
pub fn testing_logs_list(app: AppHandle) -> Vec<testlogs::LogFile> {
    testlogs::list(&game_dir(&app))
}

/// The end of one of those files for the viewer, with names in paths, account and server settings and IP addresses removed.
#[tauri::command]
pub fn testing_log_read(app: AppHandle, name: String, lines: Option<usize>) -> Result<testlogs::Tail, String> {
    testlogs::read_tail(&game_dir(&app), &name, lines)
}

/// One redacted zip of the logs in `testing-logs` next to pkp.exe. Nothing is uploaded.
#[tauri::command]
pub fn testing_logs_export(app: AppHandle, version: String) -> Result<testlogs::Exported, String> {
    testlogs::export(&game_dir(&app), &version, &load_config(&app).channel)
}

/// Opens the folder of the exported zips (made if missing), so the zip can be dragged into a GitHub issue.
#[tauri::command]
pub fn testing_logs_open_dir(app: AppHandle) -> Result<(), String> {
    let dir = game_dir(&app).join(testlogs::OUT_DIR);
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    play::open_with_shell(&dir)
}
