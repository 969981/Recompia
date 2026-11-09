// SPDX-License-Identifier: GPL-3.0-or-later
//! Choosing the game files: the dump check, saving the choice and unpacking a .nup package.

use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use tauri::Emitter;
use tauri::{AppHandle, State};

use crate::config::{load_config, save_config};
use crate::paths::game_dir_of;
use crate::setup_job::SetupState;
use crate::{dump, nup, settings, shaders, writecheck};

#[derive(Serialize, Clone)]
pub struct NupProgress {
    pub done: u64,
    pub total: u64,
    pub file: String,
}

/// Unpacks a .nup into `<game folder>/nup_dump` (base/ and updates/<version>/, the same layout as a dump folder) and
/// returns that folder. Emits "nup-progress". A finished unpack of the same package is reused.
#[tauri::command]
pub async fn import_nup(app: AppHandle, state: State<'_, Arc<SetupState>>, path: String) -> Result<String, String> {
    if state.progress.lock().unwrap().state == shaders::RunState::Running {
        return Err("Setup is running. Pause it first.".into());
    }
    let game_dir = game_dir_of(&load_config(&app));
    let out = game_dir.join("nup_dump");
    // a blocked folder fails here, in words, instead of halfway through with an OS error (writecheck.rs)
    writecheck::check(&game_dir).map_err(|e| e.message())?;
    let src = PathBuf::from(path.trim());
    let emitter = app.clone();
    let st = state.inner().clone();
    st.nup_cancel.store(false, Ordering::SeqCst);
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        nup::extract(
            &src,
            &out,
            &mut |done, total, file| {
                // the UI only needs ~100 updates over the whole unpack
                if total == 0 || done == total || done.saturating_sub(last) >= (total / 100).max(1) {
                    last = done;
                    let _ = emitter.emit("nup-progress", NupProgress { done, total, file: file.to_string() });
                }
            },
            &|| st.nup_cancel.load(Ordering::SeqCst),
        )
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(dump::fwd(&res))
}

/// Stops a running .nup unpack (the call to `import_nup` then returns an error).
#[tauri::command]
pub fn cancel_nup(state: State<Arc<SetupState>>) {
    state.nup_cancel.store(true, Ordering::SeqCst);
}

/// [fs] romfs_base for pkp.ini: the base romfs (relative to the game folder when inside it), or empty for none.
pub fn base_value(game_dir: &Path, base: &str) -> String {
    if base.is_empty() { String::new() } else { settings::portable(game_dir, base) }
}

#[tauri::command]
pub fn validate_dump(path: String) -> dump::DumpReport {
    dump::scan(&path)
}

/// Validates, stores the dump folder, and points `[fs] romfs` in pkp.ini at the chosen romfs.
#[tauri::command]
pub fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let report = dump::scan(&path);
    let chosen = report.chosen.clone().ok_or_else(|| report.errors.join(" "))?;
    let mut cfg = load_config(&app);
    cfg.dump_dir = report.path.clone();
    save_config(app.clone(), cfg.clone())?;
    settings::apply(
        &game_dir_of(&cfg),
        &[
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "exefs".into(), value: settings::portable(&game_dir_of(&cfg), &chosen.exefs) },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs".into(), value: settings::portable(&game_dir_of(&cfg), &chosen.romfs) },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs_base".into(), value: base_value(&game_dir_of(&cfg), &chosen.romfs_base) },
        ],
    )?;
    Ok(report)
}
