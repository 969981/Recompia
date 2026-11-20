// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the launcher lives and which folder is the game folder.

use std::path::{Path, PathBuf};

use crate::config::Config;

/// The launcher's folder, where `tools/` is. An AppImage runs from a temporary mount: there it is the folder of the
/// .AppImage file itself ($APPIMAGE, set by the AppImage runtime). Linux: an AppImage unpacked into a plain folder and started from
/// its AppRun has no $APPIMAGE (RecompLauncher installs every game so, its docs/DEVELOPERS.md "AppImages"); the program is then
/// <folder>/usr/bin/recompia, and the folder with tools/ is $APPDIR, set by that launcher and by AppRun's GTK hook.
pub fn exe_dir() -> PathBuf {
    let var = |name: &str| std::env::var_os(name).filter(|v| !v.is_empty()).map(PathBuf::from);
    // canonical, as current_exe is on Linux: an APPDIR given through a symlinked home (/home -> /var/home) is still ours
    let appdir = if cfg!(target_os = "linux") { var("APPDIR").and_then(|d| std::fs::canonicalize(d).ok()) } else { None };
    exe_dir_from(var("APPIMAGE"), appdir, std::env::current_exe().ok())
}

fn exe_dir_from(appimage: Option<PathBuf>, appdir: Option<PathBuf>, exe: Option<PathBuf>) -> PathBuf {
    if let Some(dir) = appimage.and_then(|p| p.parent().map(Path::to_path_buf)) {
        return dir;
    }
    // only an APPDIR our own program lives in: one inherited from another AppImage that started us says nothing about this install
    if let (Some(dir), Some(exe)) = (appdir, &exe) {
        if exe.starts_with(&dir) {
            return dir;
        }
    }
    exe.and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| PathBuf::from("."))
}

/// The game folder a fresh install starts from: the launcher's own folder, or -- inside a macOS `.app` -- the folder the bundle
/// sits in (the bundle carries the launcher only; pkp, pkp.ini and shader_cache stay beside it, where a 26 GB cache belongs).
pub fn default_game_dir() -> PathBuf {
    let exe = exe_dir();
    if exe.file_name().is_some_and(|n| n == "MacOS") && exe.parent().is_some_and(|p| p.file_name().is_some_and(|n| n == "Contents")) {
        let beside = exe.parent().and_then(std::path::Path::parent).and_then(std::path::Path::parent);
        // a game folder kept beside the bundle (development) wins over the installed one
        if let Some(dir) = &beside {
            if dir.join(crate::settings::PKP_INI).is_file() {
                return dir.to_path_buf();
            }
        }
        // Installed into /Applications: the bundle is read only, so the game folder (pkp, pkp.ini, shader_cache, saves)
        // lives in Application Support, which Set up creates.
        if let Some(home) = std::env::var_os("HOME").map(PathBuf::from) {
            return home.join("Library").join("Application Support").join("Recompia").join("game");
        }
        if let Some(dir) = beside {
            return dir.to_path_buf();
        }
    }
    exe
}

pub fn game_dir_of(cfg: &Config) -> PathBuf {
    // An install RecompLauncher keeps is its own game folder (managed.rs). launcher.json is shared by every install on the machine,
    // and an Install folder chosen in another one must not send this one's Set up and Play there.
    if crate::managed::by_recomplauncher() {
        return default_game_dir();
    }
    let chosen = cfg.game_dir.trim();
    // A game folder inside an .app is never the real one: an earlier build offered exe_dir() (the bundle's Contents) as the
    // default, and saving the settings wrote it back. Ignore it, so the next save heals the file.
    if chosen.is_empty() || chosen.contains(".app/") {
        default_game_dir()
    } else {
        PathBuf::from(chosen)
    }
}

/// The launcher file itself: the AppImage, not the program inside its mount.
pub fn launcher_path() -> Option<PathBuf> {
    std::env::var_os("APPIMAGE").map(PathBuf::from).or_else(|| std::env::current_exe().ok())
}

#[cfg(test)]
mod tests {
    use super::exe_dir_from;
    use std::path::PathBuf;

    fn p(s: &str) -> Option<PathBuf> {
        Some(PathBuf::from(s))
    }

    #[test]
    fn a_mounted_appimage_is_the_folder_of_its_file() {
        let dir = exe_dir_from(p("/games/Recompia/Recompia.AppImage"), p("/tmp/.mount_RecompXYZ"), p("/tmp/.mount_RecompXYZ/usr/bin/recompia"));
        assert_eq!(dir, PathBuf::from("/games/Recompia"));
    }

    #[test]
    fn an_unpacked_appimage_is_its_appdir() {
        let dir = exe_dir_from(None, p("/games/Recompia"), p("/games/Recompia/usr/bin/recompia"));
        assert_eq!(dir, PathBuf::from("/games/Recompia"));
    }

    #[test]
    fn an_appdir_the_program_is_not_in_is_ignored() {
        let dir = exe_dir_from(None, p("/tmp/.mount_OtherApp"), p("/games/Recompia/recompia"));
        assert_eq!(dir, PathBuf::from("/games/Recompia"));
    }

    #[test]
    fn a_plain_program_is_its_own_folder() {
        let dir = exe_dir_from(None, None, p("/games/Recompia/recompia"));
        assert_eq!(dir, PathBuf::from("/games/Recompia"));
    }
}
