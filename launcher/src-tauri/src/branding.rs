// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's icon and Steam art, Stable or Testing (the Testing channel has its own icons). The pages show their own pictures
//! (src/branding.js); this side owns the art the system shows:
//!  * this window's icon (title bar, taskbar, Alt+Tab)
//!  * on Windows, the icon inside pkp.exe (Explorer, shortcuts, the game window and its taskbar button) and inside the launcher's exe
//!  * the Steam shortcut art (steam.rs asks `steam_art`; `apply` refreshes the art of shortcuts already added)
//!
//! The exes are checked again at every launcher start and every Play: an update or Set up writes new exes with the default icon, and
//! an exe that still carries the icon of an older launcher gets the current one the same way.

use std::path::Path;
use tauri::{AppHandle, Manager};

use crate::config::Config;

/// Whether the Testing icons apply (the Testing channel). launcher.json `logo` / `icon` of older launchers are no longer read.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Branding {
    pub testing: bool,
}

impl Branding {
    pub fn of(cfg: &Config) -> Branding {
        Branding { testing: cfg.channel == "testing" }
    }
}

// ---- the art ------------------------------------------------------------------------------------------------------------------------

/// The .ico for an exe (Windows only: an AppImage or a macOS app keeps the icon it was built with).
#[cfg_attr(not(windows), allow(dead_code))]
pub fn ico(testing: bool) -> &'static [u8] {
    if testing {
        include_bytes!("../icons-testing/icon.ico")
    } else {
        include_bytes!("../icons/icon.ico")
    }
}

/// The PNG for this window's icon.
fn window_png(testing: bool) -> &'static [u8] {
    if testing {
        include_bytes!("../icons-testing/icon.png")
    } else {
        include_bytes!("../icons/icon.png")
    }
}

/// One picture of the Steam shortcut (steam.rs art_files kinds: wide, portrait, hero, logo, icon).
fn steam_art_for(kind: &str, testing: bool) -> &'static [u8] {
    match (kind, testing) {
        ("hero", _) => include_bytes!("../assets/steam/hero.png"),
        ("wide", _) => include_bytes!("../assets/steam/grid_wide.png"),
        ("portrait", _) => include_bytes!("../assets/steam/grid_portrait.png"),
        ("logo", _) => include_bytes!("../assets/steam/logo.png"),
        (_, false) => include_bytes!("../assets/steam/icon.png"),
        (_, true) => include_bytes!("../assets/steam/icon_testing.png"),
    }
}

/// The picture the Steam shortcut gets now.
pub fn steam_art(kind: &str, b: &Branding) -> &'static [u8] {
    steam_art_for(kind, b.testing)
}

/// Whether `bytes` is the picture this launcher writes for `kind`: only that is ours to replace or delete.
pub fn is_our_steam_art(kind: &str, testing: bool, bytes: &[u8]) -> bool {
    steam_art_for(kind, testing) == bytes
}

// ---- putting it in place ------------------------------------------------------------------------------------------------------------

/// This window's icon. macOS has no window icon (the Dock shows the app's).
pub fn apply_window(app: &AppHandle, b: &Branding) {
    let Some(window) = app.get_webview_window("main") else { return };
    match tauri::image::Image::from_bytes(window_png(b.testing)) {
        Ok(image) => {
            let _ = window.set_icon(image);
        }
        Err(e) => eprintln!("branding: the window icon could not be read: {e}"),
    }
}

/// The game exe in `game_dir` shows the channel's icon (Windows; elsewhere the game binary has no icon of its own). A missing exe is fine:
/// Set up writes one later, and Play checks again. Fails when the exe is running or cannot be written.
pub fn ensure_game_icon(game_dir: &Path, b: &Branding) -> Result<(), String> {
    #[cfg(windows)]
    {
        let exe = crate::host::game_exe(game_dir);
        let ico = ico(b.testing);
        if exe.is_file() && !crate::exe_icon::has_icon(&exe, ico) {
            crate::exe_icon::stamp(&exe, ico)?;
            crate::exe_icon::notify_shell(&exe);
        }
    }
    #[cfg(not(windows))]
    let _ = (game_dir, b);
    Ok(())
}

/// The launcher's own exe shows the channel's icon (Windows; an AppImage or a macOS app keeps the icon it was built with).
pub fn ensure_launcher_icon(b: &Branding) -> Result<(), String> {
    #[cfg(windows)]
    if let Some(exe) = crate::paths::launcher_path() {
        let ico = ico(b.testing);
        if !crate::exe_icon::has_icon(&exe, ico) {
            crate::exe_icon::swap_running(&exe, &crate::update_apply::with_old(&exe), ico)?;
            crate::exe_icon::notify_shell(&exe);
        }
    }
    #[cfg(not(windows))]
    let _ = b;
    Ok(())
}

/// Everything after the player changed the channel. Each place is tried on its own; what failed comes back as one message (the game
/// exe while the game runs, for one: Play fixes it before the next start).
pub fn apply(app: &AppHandle, cfg: &Config) -> Result<(), String> {
    let b = Branding::of(cfg);
    apply_window(app, &b);
    let mut failed = Vec::new();
    if let Err(e) = ensure_game_icon(&crate::paths::game_dir_of(cfg), &b) {
        failed.push(e);
    }
    if let Err(e) = ensure_launcher_icon(&b) {
        failed.push(e);
    }
    if let Err(e) = crate::steam::refresh_art(&b) {
        failed.push(e);
    }
    if failed.is_empty() {
        Ok(())
    } else {
        Err(failed.join(" "))
    }
}

/// A launcher start: the window, then both exes (an update or Set up may have written them with the default icon since). Runs on the
/// cleanup thread after update::clean_start, so the `.old` of an update is gone before the launcher's own swap needs that name.
pub fn on_start(cfg: &Config) {
    let b = Branding::of(cfg);
    for result in [ensure_game_icon(&crate::paths::game_dir_of(cfg), &b), ensure_launcher_icon(&b)] {
        if let Err(e) = result {
            eprintln!("branding: {e}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn testing_has_its_own_icons() {
        let stable = Branding { testing: false };
        let testing = Branding { testing: true };
        assert_ne!(steam_art("icon", &stable), steam_art("icon", &testing));
        assert_eq!(steam_art("logo", &stable), steam_art("logo", &testing));
        assert_ne!(ico(false), ico(true));
        assert_ne!(window_png(false), window_png(true));
    }

    #[test]
    fn our_art_is_ours_and_a_players_own_art_is_not() {
        for kind in ["wide", "portrait", "hero", "logo", "icon"] {
            assert!(is_our_steam_art(kind, false, steam_art_for(kind, false)), "{kind}");
            assert!(!is_our_steam_art(kind, false, b"a picture the player chose"));
        }
    }
}
