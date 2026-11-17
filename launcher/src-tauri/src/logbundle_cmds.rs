// SPDX-License-Identifier: GPL-3.0-or-later
//! The command behind "Save logs for a bug report" (logbundle.rs builds the zip).

use serde::Serialize;
use std::path::{Path, PathBuf};
use tauri::{AppHandle, Manager};

use crate::config::game_dir;
use crate::preflight::{self, Vulkan};
use crate::{coop, host, logbundle, play};

#[derive(Serialize)]
pub struct Saved {
    /// The zip, with forward slashes.
    pub path: String,
}

/// launcher version, OS, CPU, RAM and GPUs, one fact a line.
fn system_text() -> String {
    let (vulkan, ram, cpu) = preflight::machine();
    let mut t = format!("Recompia launcher {}\nOS: {} ({})\n", env!("CARGO_PKG_VERSION"), host::os_name(), std::env::consts::ARCH);
    t.push_str(&format!("CPU: {}\n", cpu.unwrap_or_else(|| "unknown".into())));
    if let Some(r) = ram {
        t.push_str(&format!("RAM: {} GiB\n", r >> 30));
    }
    match vulkan {
        Vulkan::Gpus(g) => g.iter().for_each(|g| t.push_str(&format!("GPU: {} (Vulkan {}.{}{})\n", g.name, g.api.0, g.api.1, if g.software { ", software" } else { "" }))),
        Vulkan::NoLoader(e) | Vulkan::Failed(e) => t.push_str(&format!("GPU: not found ({})\n", e)),
    }
    t
}

/// Downloads, else the desktop, else the game folder.
fn target_dir(app: &AppHandle, game: &Path) -> PathBuf {
    let p = app.path();
    [p.download_dir(), p.desktop_dir()].into_iter().flatten().find(|d| d.is_dir()).unwrap_or_else(|| game.to_path_buf())
}

/// Writes `recompia-logs-<time>.zip` where the player finds it, shows the folder and says where the file is.
#[tauri::command]
pub async fn save_log_bundle(app: AppHandle) -> Result<Saved, String> {
    let game = game_dir(&app);
    let out = target_dir(&app, &game).join(format!("recompia-logs-{}.zip", chrono::Local::now().format("%Y%m%d-%H%M%S")));
    let path = out.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let zip = logbundle::build(&game, &coop::root_of(&game), &system_text())?;
        std::fs::write(&path, zip).map_err(|e| format!("Cannot write {}: {}", crate::dump::fwd(&path), e))
    })
    .await
    .map_err(|e| e.to_string())??;
    // the folder opens as a convenience: a file manager that does not start is not a failed save
    let _ = out.parent().map(play::open_with_shell);
    Ok(Saved { path: crate::dump::fwd(&out) })
}
