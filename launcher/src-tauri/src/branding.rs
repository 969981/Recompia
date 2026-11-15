// SPDX-License-Identifier: GPL-3.0-or-later
//! The player's logo and icon (launcher.json `logo` and `icon`, Settings > Game > Logo and icon; #557, #621). "classic" is the
//! bubble-letter wordmark and the R icon the launcher had before 0.11.1; anything else (empty, a value from a newer launcher) is the
//! current Recompia art. The pages switch their own pictures (src/branding.js); this side owns the art the system shows:
//!  * this window's icon (title bar, taskbar, Alt+Tab)
//!  * on Windows, the icon inside pkp.exe (Explorer, shortcuts, the game window and its taskbar button) and inside the launcher's exe
//!  * the Steam shortcut art (steam.rs asks `steam_art`; `apply` refreshes the art of shortcuts already added)
//! The exes are checked again at every launcher start and every Play: an update or Set up writes new exes with the default icon.

use std::path::Path;
use tauri::{AppHandle, Manager};

use crate::config::Config;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Look {
    Recompia,
    Classic,
}

impl Look {
    pub fn parse(s: &str) -> Look {
        if s.trim().eq_ignore_ascii_case("classic") {
            Look::Classic
        } else {
            Look::Recompia
        }
    }
}

/// The two choices, and whether the Testing icons apply (the Testing channel, as for the Steam art before).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Branding {
    pub logo: Look,
    pub icon: Look,
    pub testing: bool,
}

impl Branding {
    pub fn of(cfg: &Config) -> Branding {
        Branding { logo: Look::parse(&cfg.logo), icon: Look::parse(&cfg.icon), testing: cfg.channel == "testing" }
    }
}

// ---- the art ------------------------------------------------------------------------------------------------------------------------
// Recompia: the files the bundle and the Steam shortcut used before the choice existed. Classic: assets/branding/classic, the 0.10 files
// (icons, Steam logo and icons) and the Koi Steam grids with the classic wordmark in place of the new one.

/// The .ico for an exe (Windows only: an AppImage or a macOS app keeps the icon it was built with).
#[cfg_attr(not(windows), allow(dead_code))]
pub fn ico(icon: Look, testing: bool) -> &'static [u8] {
    match (icon, testing) {
        (Look::Recompia, false) => include_bytes!("../icons/icon.ico"),
        (Look::Recompia, true) => include_bytes!("../icons-testing/icon.ico"),
        (Look::Classic, false) => include_bytes!("../assets/branding/classic/icon.ico"),
        (Look::Classic, true) => include_bytes!("../assets/branding/classic/icon_testing.ico"),
    }
}

/// The PNG for this window's icon.
fn window_png(icon: Look, testing: bool) -> &'static [u8] {
    match (icon, testing) {
        (Look::Recompia, false) => include_bytes!("../icons/icon.png"),
        (Look::Recompia, true) => include_bytes!("../icons-testing/icon.png"),
        (Look::Classic, false) => include_bytes!("../assets/branding/classic/window.png"),
        (Look::Classic, true) => include_bytes!("../assets/branding/classic/window_testing.png"),
    }
}

const HERO: &[u8] = include_bytes!("../assets/steam/hero.png");

/// One picture of the Steam shortcut (steam.rs art_files kinds: wide, portrait, hero, logo, icon) for `logo` / `icon` choices.
fn steam_art_for(kind: &str, logo: Look, icon: Look, testing: bool) -> &'static [u8] {
    match (kind, logo, icon, testing) {
        ("hero", ..) => HERO,
        ("wide", Look::Recompia, ..) => include_bytes!("../assets/steam/grid_wide.png"),
        ("wide", Look::Classic, ..) => include_bytes!("../assets/branding/classic/steam_grid_wide.png"),
        ("portrait", Look::Recompia, ..) => include_bytes!("../assets/steam/grid_portrait.png"),
        ("portrait", Look::Classic, ..) => include_bytes!("../assets/branding/classic/steam_grid_portrait.png"),
        ("logo", Look::Recompia, ..) => include_bytes!("../assets/steam/logo.png"),
        ("logo", Look::Classic, ..) => include_bytes!("../assets/branding/classic/steam_logo.png"),
        (_, _, Look::Recompia, false) => include_bytes!("../assets/steam/icon.png"),
        (_, _, Look::Recompia, true) => include_bytes!("../assets/steam/icon_testing.png"),
        (_, _, Look::Classic, false) => include_bytes!("../assets/branding/classic/steam_icon.png"),
        (_, _, Look::Classic, true) => include_bytes!("../assets/branding/classic/steam_icon_testing.png"),
    }
}

/// The picture the Steam shortcut gets now.
pub fn steam_art(kind: &str, b: &Branding) -> &'static [u8] {
    steam_art_for(kind, b.logo, b.icon, b.testing)
}

/// Whether `bytes` is one of the pictures this launcher writes for `kind` (any choice): only those are ours to replace or delete.
pub fn is_our_steam_art(kind: &str, testing: bool, bytes: &[u8]) -> bool {
    [Look::Recompia, Look::Classic].iter().any(|&l| steam_art_for(kind, l, l, testing) == bytes)
}

// ---- putting it in place ------------------------------------------------------------------------------------------------------------

/// This window's icon. macOS has no window icon (the Dock shows the app's).
pub fn apply_window(app: &AppHandle, b: &Branding) {
    let Some(window) = app.get_webview_window("main") else { return };
    match tauri::image::Image::from_bytes(window_png(b.icon, b.testing)) {
        Ok(image) => {
            let _ = window.set_icon(image);
        }
        Err(e) => eprintln!("branding: the window icon could not be read: {e}"),
    }
}

/// The game exe in `game_dir` shows the chosen icon (Windows; elsewhere the game binary has no icon of its own). A missing exe is fine:
/// Set up writes one later, and Play checks again. Fails when the exe is running or cannot be written.
pub fn ensure_game_icon(game_dir: &Path, b: &Branding) -> Result<(), String> {
    #[cfg(windows)]
    {
        let exe = crate::host::game_exe(game_dir);
        let ico = ico(b.icon, b.testing);
        if exe.is_file() && !crate::exe_icon::has_icon(&exe, ico) {
            crate::exe_icon::stamp(&exe, ico)?;
            crate::exe_icon::notify_shell(&exe);
        }
    }
    #[cfg(not(windows))]
    let _ = (game_dir, b);
    Ok(())
}

/// The launcher's own exe shows the chosen icon (Windows; an AppImage or a macOS app keeps the icon it was built with).
pub fn ensure_launcher_icon(b: &Branding) -> Result<(), String> {
    #[cfg(windows)]
    if let Some(exe) = crate::paths::launcher_path() {
        let ico = ico(b.icon, b.testing);
        if !crate::exe_icon::has_icon(&exe, ico) {
            crate::exe_icon::swap_running(&exe, &crate::update_apply::with_old(&exe), ico)?;
            crate::exe_icon::notify_shell(&exe);
        }
    }
    #[cfg(not(windows))]
    let _ = b;
    Ok(())
}

/// Everything after the player changed a choice. Each place is tried on its own; what failed comes back as one message (the game
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
    fn only_classic_is_classic() {
        assert_eq!(Look::parse(" Classic "), Look::Classic);
        for s in ["", "recompia", "new", "classic2"] {
            assert_eq!(Look::parse(s), Look::Recompia, "{s}");
        }
    }

    #[test]
    fn logo_and_icon_choose_their_own_pictures() {
        let b = Branding { logo: Look::Classic, icon: Look::Recompia, testing: false };
        assert_eq!(steam_art("logo", &b), steam_art_for("logo", Look::Classic, Look::Classic, false));
        assert_eq!(steam_art("wide", &b), steam_art_for("wide", Look::Classic, Look::Classic, false));
        assert_eq!(steam_art("icon", &b), steam_art_for("icon", Look::Recompia, Look::Recompia, false));
        assert_ne!(steam_art("icon", &b), steam_art("icon", &Branding { testing: true, ..b }), "Testing has its own icon");
        assert_ne!(ico(Look::Recompia, false), ico(Look::Classic, false));
    }

    #[test]
    fn either_choice_is_ours_and_a_players_own_art_is_not() {
        for kind in ["wide", "portrait", "hero", "logo", "icon"] {
            for l in [Look::Recompia, Look::Classic] {
                assert!(is_our_steam_art(kind, false, steam_art_for(kind, l, l, false)), "{kind}");
            }
            assert!(!is_our_steam_art(kind, false, b"a picture the player chose"));
        }
    }
}
