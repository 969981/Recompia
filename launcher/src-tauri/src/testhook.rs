// SPDX-License-Identifier: GPL-3.0-or-later
//! Debug builds only: lets a test run drive the page without input (PKP_LAUNCHER_EVAL_FILE) and report back (test_report). A release
//! build has neither, so a launcher started with that variable set runs nothing from the file.

use tauri::Manager;

/// Test runs drive the page without input: PKP_LAUNCHER_EVAL_FILE=<path>. A JS file written there is run in the page once and
/// deleted; it reports back through document.title (the window title). WebView2's DevTools port does not open for Tauri apps here.
pub fn drive_page(app: &tauri::App) {
    if let Ok(path) = std::env::var("PKP_LAUNCHER_EVAL_FILE") {
        if let Some(win) = app.get_webview_window("main") {
            std::thread::spawn(move || loop {
                std::thread::sleep(std::time::Duration::from_millis(300));
                if let Ok(js) = std::fs::read_to_string(&path) {
                    let _ = std::fs::remove_file(&path);
                    let _ = win.eval(&js);
                }
            });
        }
    }
}

/// Test runs only (PKP_LAUNCHER_EVAL_FILE set): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
pub fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var("PKP_LAUNCHER_EVAL_FILE").map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}

/// Debug builds only: PKP_ZONITH_RELEASES_URL points Zonith at a stand-in release list, so a test run can download, check and start a
/// local setup (zonith_cmds.rs). A release build always reads GitHub's.
pub fn zonith_releases_url() -> Option<String> {
    if cfg!(debug_assertions) { std::env::var("PKP_ZONITH_RELEASES_URL").ok() } else { None }
}
