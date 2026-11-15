// SPDX-License-Identifier: GPL-3.0-or-later
//! Shader storage jobs: "Make shader files smaller" and its undo.

use serde::Serialize;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tauri::Emitter;
use tauri::AppHandle;

use crate::states::{StorageArg, States};
use crate::config::game_dir;
use crate::guard::{ensure_idle, Need};
use crate::{preflight, setuplog, shader_archive, shaders, writecheck};

/// "Make shader files smaller" and its undo (shader_archive.rs): one at a time, never while the game, setup or an update runs.
#[derive(Default)]
pub struct StorageState {
    pub running: AtomicBool,
    pub cancel: AtomicBool,
}

#[derive(Serialize, Clone)]
pub struct StorageProgress {
    /// "compact" or "expand"
    pub kind: String,
    pub done: u64,
    pub total: u64,
}

#[derive(Serialize)]
pub struct StorageStatus {
    #[serde(flatten)]
    pub storage: shader_archive::Storage,
    /// setup finished, so a move now is the whole cache (the launcher offers it); during or before setup it is not offered
    pub complete: bool,
    pub running: bool,
}

/// How the translated shaders are stored. Lists the cache folder, so it runs off the UI thread.
#[tauri::command]
pub async fn shader_storage(app: AppHandle, state: StorageArg<'_>) -> Result<StorageStatus, String> {
    let dir = game_dir(&app);
    let running = state.running.load(Ordering::SeqCst);
    tauri::async_runtime::spawn_blocking(move || StorageStatus { storage: shader_archive::storage(&dir), complete: shaders::is_complete(&dir), running })
        .await
        .map_err(|e| e.to_string())
}

/// Starts a storage job after the checks every one of them needs. The guard clears `running` however the job ends.
pub fn begin_storage(app: &AppHandle) -> Result<(PathBuf, Arc<StorageState>), String> {
    ensure_idle(app, &[
        Need::Update,
        Need::Setup("Setup is running. Wait for it to finish, or pause it first."),
        Need::Game("The game is running. Close it first: it reads the shader files while it runs."),
    ])?;
    let st = app.storage().inner().clone();
    if st.running.swap(true, Ordering::SeqCst) {
        return Err("The shader files are already being repacked.".into());
    }
    st.cancel.store(false, Ordering::SeqCst);
    Ok((game_dir(app), st))
}

pub struct StorageRun(Arc<StorageState>);
impl Drop for StorageRun {
    fn drop(&mut self) {
        self.0.running.store(false, Ordering::SeqCst);
    }
}

#[derive(Serialize)]
pub struct CompactResult {
    #[serde(flatten)]
    pub moved: shader_archive::Moved,
    pub finished: bool,
}

/// Moves every loose shader file into the shader archive. Nothing is translated: the files are compressed as they are. Emits
/// "shader-storage-progress"; resolves when done or stopped (cancel_shader_storage). What was moved stays moved.
#[tauri::command]
pub async fn compact_shaders(app: AppHandle) -> Result<CompactResult, String> {
    let (dir, st) = begin_storage(&app)?;
    let run = StorageRun(st.clone());
    let slog = setuplog::SetupLog::open(&dir);
    let (moved, finished) = tauri::async_runtime::spawn_blocking(move || {
        let _run = run;
        writecheck::check(&dir).map_err(|e| e.message())?;
        let before = shader_archive::storage(&dir);
        slog.line(&format!("=== Make shader files smaller: {} loose files ({:.1} GB)", before.loose_files, before.loose_bytes as f64 / preflight::GB as f64));
        let r = shader_archive::compact(&dir, None, None, &st.cancel, |done, total| {
            let _ = app.emit("shader-storage-progress", StorageProgress { kind: "compact".into(), done, total });
        });
        // a later Set up again keeps the cache packed (shaders.rs Marker::storage)
        if r.is_ok() {
            shaders::set_storage(&dir, "compact")?;
        }
        match &r {
            Ok((m, fin)) => slog.line(&format!(
                "=== {}: {} shaders, {:.2} GB of SPIR-V in {:.2} GB ({:.2}x), {:.0} s",
                if *fin { "Done" } else { "Stopped" },
                m.shaders,
                m.raw_bytes as f64 / preflight::GB as f64,
                m.disk_bytes as f64 / preflight::GB as f64,
                if m.disk_bytes > 0 { m.raw_bytes as f64 / m.disk_bytes as f64 } else { 0.0 },
                m.seconds
            )),
            Err(e) => slog.line(&format!("=== Make shader files smaller failed: {}", e)),
        }
        r
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(CompactResult { moved, finished })
}

/// The undo: writes the archive back out as loose files. Needs the free space of the loose files (about 26 GB).
#[tauri::command]
pub async fn expand_shaders(app: AppHandle) -> Result<bool, String> {
    let (dir, st) = begin_storage(&app)?;
    let run = StorageRun(st.clone());
    tauri::async_runtime::spawn_blocking(move || {
        let _run = run;
        let s = shader_archive::storage(&dir);
        // segments are deleted as they are written out: the peak is the loose size less what the archive already took
        let need = s.archive_raw_bytes.saturating_sub(s.archive_bytes) + preflight::GB;
        if let Some(free) = preflight::free_bytes(&dir) {
            if free < need {
                return Err(format!(
                    "Loose shader files need about {:.0} GB more free space on {} ({:.0} GB free).",
                    need as f64 / preflight::GB as f64,
                    preflight::drive_of(&dir),
                    free as f64 / preflight::GB as f64
                ));
            }
        }
        let finished = shader_archive::expand(&dir, &st.cancel, |done, total| {
            let _ = app.emit("shader-storage-progress", StorageProgress { kind: "expand".into(), done, total });
        })?;
        if finished {
            shaders::set_storage(&dir, "loose")?;
        }
        Ok(finished)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
pub fn cancel_shader_storage(state: StorageArg) {
    state.cancel.store(true, Ordering::SeqCst);
}
