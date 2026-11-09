// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the launcher lives and which folder is the game folder.

use std::path::{Path, PathBuf};

use crate::config::Config;

/// The launcher's folder, where `tools/` is. An AppImage runs from a temporary mount: there it is the folder of the
/// .AppImage file itself ($APPIMAGE, set by the AppImage runtime).
pub fn exe_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("APPIMAGE").map(PathBuf::from).and_then(|p| p.parent().map(Path::to_path_buf)) {
        return dir;
    }
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

pub fn game_dir_of(cfg: &Config) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        exe_dir()
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

/// The launcher file itself: the AppImage, not the program inside its mount.
pub fn launcher_path() -> Option<PathBuf> {
    std::env::var_os("APPIMAGE").map(PathBuf::from).or_else(|| std::env::current_exe().ok())
}
