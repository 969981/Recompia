// SPDX-License-Identifier: GPL-3.0-or-later
//! The command the test runs report through (PKP_LAUNCHER_EVAL_FILE).

/// Test runs only (PKP_LAUNCHER_EVAL_FILE set): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
pub fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var("PKP_LAUNCHER_EVAL_FILE").map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}
