// SPDX-License-Identifier: GPL-3.0-or-later
//! Choosing the game files: the dump check, saving the choice and unpacking a game package (.nup or .nx).

use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use tauri::Emitter;
use tauri::AppHandle;

use crate::states::SetupArg;
use crate::config::{game_dir, load_config, save_config};
use crate::error::{self, CmdError};
use crate::guard::{ensure_idle, Need};
use crate::paths::game_dir_of;
use crate::{dump, nsp, settings, unpack, writecheck};

#[derive(Serialize, Clone)]
pub struct UnpackProgress {
    pub done: u64,
    pub total: u64,
    pub file: String,
}

/// Unpacks a .nup or an .nx into `<game folder>/nup_dump` (base/ and updates/<version>/, the same layout as a dump
/// folder; the folder keeps the name it got when .nup was the only package) and returns that folder. Emits
/// "unpack-progress". A finished unpack of the same package is reused; another package replaces it.
#[tauri::command]
pub async fn import_package(app: AppHandle, state: SetupArg<'_>, path: String) -> Result<String, CmdError> {
    ensure_idle(&app, &[Need::Setup("Setup is running. Pause it first.")])?;
    let game_dir = game_dir(&app);
    let out = game_dir.join("nup_dump");
    // a blocked folder fails here, in words, instead of halfway through with an OS error (writecheck.rs)
    writecheck::check(&game_dir).map_err(|e| e.message())?;
    let src = PathBuf::from(path.trim());
    let emitter = app.clone();
    let st = state.inner().clone();
    st.unpack_cancel.store(false, Ordering::SeqCst);
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        unpack::extract(
            &src,
            &out,
            &mut |done, total, file| {
                // the UI only needs ~100 updates over the whole unpack
                if total == 0 || done == total || done.saturating_sub(last) >= (total / 100).max(1) {
                    last = done;
                    let _ = emitter.emit("unpack-progress", UnpackProgress { done, total, file: file.to_string() });
                }
            },
            &|| st.unpack_cancel.load(Ordering::SeqCst),
        )
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|m| CmdError::coded_if(m, unpack::CANCELLED, error::UNPACK_CANCELLED))?;
    Ok(dump::fwd(&res))
}

/// Unpacks the add-on (DLC) in a decrypted .nsp into the folder `[fs] aoc_dir` names (`<game folder>/aoc` when pkp.ini names none,
/// which is then written there) and returns the add-on ids. The game lists them at its next start.
#[tauri::command]
pub async fn import_dlc(app: AppHandle, path: String) -> Result<Vec<String>, String> {
    ensure_idle(&app, &[Need::Setup("Setup is running. Pause it first.")])?;
    let game_dir = game_dir(&app);
    writecheck::check(&game_dir).map_err(|e| e.message())?;
    let src = PathBuf::from(path.trim());
    tauri::async_runtime::spawn_blocking(move || nsp::extract(&src, &settings::aoc_target(&game_dir)?))
        .await
        .map_err(|e| e.to_string())?
}

/// Stops a running package unpack (the call to `import_package` then returns an error).
#[tauri::command]
pub fn cancel_unpack(state: SetupArg) {
    state.unpack_cancel.store(true, Ordering::SeqCst);
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
