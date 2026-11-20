// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's icon and Steam art: the launcher's own, Stable or Testing (the Experimental channel has the Testing icons), or the
//! pictures of the theme the player chose (themes.rs) where it has them. The pages show their own pictures (src/branding.js); this side
//! owns the art the system shows:
//!  * this window's icon (title bar, taskbar, Alt+Tab)
//!  * on Windows, the icon inside pkp.exe (Explorer, shortcuts, the game window and its taskbar button) and inside the launcher's exe
//!  * the Steam shortcut art (steam.rs asks `steam_art`; `apply` refreshes the art of shortcuts already added)
//!
//! The exes are checked again at every launcher start and every Play: an update or Set up writes new exes with the default icon, and
//! an exe that still carries the icon of an older launcher gets the current one the same way.

use sha2::{Digest, Sha256};
use std::path::Path;
use std::sync::Arc;
use tauri::{AppHandle, Manager};

use crate::config::Config;
use crate::themes::{self, Art};

/// Whether the Testing icons apply (the Experimental channel), and the player's theme art when a theme with pictures is chosen
/// (launcher.json `theme`). launcher.json `logo` / `icon` of older launchers are no longer read.
#[derive(Clone, Debug, PartialEq)]
pub struct Branding {
    pub testing: bool,
    pub art: Option<Arc<Art>>,
}

impl Branding {
    pub fn of(cfg: &Config) -> Branding {
        let testing = crate::releases::Channel::parse(&cfg.channel) == crate::releases::Channel::Experimental;
        let art = Some(cfg.theme.as_str()).filter(|k| !k.is_empty()).and_then(themes::load_art);
        Branding { testing, art }
    }

    /// The .ico for an exe (Windows only: an AppImage or a macOS app keeps the icon it was built with).
    #[cfg_attr(not(windows), allow(dead_code))]
    pub fn ico(&self) -> &[u8] {
        self.art.as_ref().and_then(|a| a.ico.as_deref()).unwrap_or_else(|| ico(self.testing))
    }

    /// The PNG for this window's icon.
    fn window_png(&self) -> &[u8] {
        self.art.as_ref().and_then(|a| a.window_png.as_deref()).unwrap_or_else(|| window_png(self.testing))
    }

    /// The picture the Steam shortcut gets now (steam.rs art_files kinds: wide, portrait, hero, logo, icon).
    pub fn steam_art(&self, kind: &str) -> &[u8] {
        self.art.as_ref().and_then(|a| a.steam.get(kind)).map_or_else(|| steam_art_for(kind, self.testing), Vec::as_slice)
    }

    /// Whether `bytes` is a picture this launcher wrote for `kind`, now or in an older version, from the launcher's own art or from a
    /// theme (the ledger in themes.rs): only that is ours to replace or delete. A picture the player chose in Steam is none of these.
    pub fn is_our_steam_art(&self, kind: &str, bytes: &[u8]) -> bool {
        self.steam_art(kind) == bytes || is_our_builtin_art(kind, self.testing, bytes) || themes::ledger_has(kind, bytes)
    }

    /// Notes a theme's picture in the ledger before it is written to Steam (the launcher's own pictures are known by their bytes).
    pub fn note_steam_art(&self, kind: &str) -> Result<(), String> {
        match self.art.as_ref().and_then(|a| a.steam.get(kind)) {
            Some(bytes) => themes::ledger_add(kind, bytes),
            None => Ok(()),
        }
    }
}

// ---- the art ------------------------------------------------------------------------------------------------------------------------

/// The launcher's own .ico for an exe.
#[cfg_attr(not(windows), allow(dead_code))]
fn ico(testing: bool) -> &'static [u8] {
    if testing {
        include_bytes!("../icons-testing/icon.ico")
    } else {
        include_bytes!("../icons/icon.ico")
    }
}

/// The launcher's own PNG for this window's icon.
fn window_png(testing: bool) -> &'static [u8] {
    if testing {
        include_bytes!("../icons-testing/icon.png")
    } else {
        include_bytes!("../icons/icon.png")
    }
}

/// One of the launcher's own pictures of the Steam shortcut.
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

/// SHA-256 of every picture an earlier launcher wrote into a player's Steam grid folder (the earlier default art and the removed
/// Classic set), as (kind, hash). Taken from the git history of launcher/src-tauri/assets/steam and assets/branding/classic. The kind
/// is the steam.rs art kind, except the Testing icon is "icon_testing". When a picture is replaced by a new version, append the hash
/// of the version it replaces here (`sha256sum` of the old file), so players who still have it get the new one.
const OLD_STEAM_ART: &[(&str, &str)] = &[
    ("hero", "335d885e2f282f846ca840c6640e452b4860283f3d723326440888f0813a921a"),
    ("hero", "416265e5a6b4afcd43c5920e428d42401b13c6f4d9e8a31e0a061963c05109ba"),
    ("hero", "762af3ac7aeb2ec9d59739c01d0edfe627389bd94cf26a75b68e9edf91693923"),
    ("hero", "a90546bd93d06c409644994f3f5963c97e8dae0234dabce7f91ca44a87652d95"),
    ("hero", "dd6d26b04937611f422047191a856d5f455377369bd772f48d7d12c58d1acd43"),
    ("icon", "0553f964722b9176c2eebf527b44ddd606c5f0d9f379ef48ede7b7a40cfa1e29"),
    ("icon", "0acb3f565dcd0b9b4559d9698c870d752a7b3d8be8fda4d905b20fb083ce5ba1"),
    ("icon", "42a93f90889b97c599376cb9fa09f6ee423b075ccd179b1976dca8bcdbdb9cb5"),
    ("icon", "521fff04acfd4742f852ba6347faadb661da47e9999a5a4fd9613a2c723e0bdb"),
    ("icon", "985e6445df527c5eeda962dff1c78191a78042f4451f2eeefcabfa569cc9049b"),
    ("icon_testing", "15a8998a6f2f0d6d6005840322497456e2962427fa22b000435b8bd5969eb214"),
    ("icon_testing", "18dcf3d12e0402003b8200450b7ae3ce9bdf08fd05706a87d3493e1861d49457"),
    ("icon_testing", "39323c6d29d4de08cfd9a5105740b0fc2cfa32ae97594d2a5921120c76e986b3"),
    ("icon_testing", "5554023917266efa1395c55b3e9b4d70d3d9410fc8fd7246ac9c0cc3f1472c83"),
    ("icon_testing", "b2b5bd9bf2e329c8e5a1349360cc1f003ab35ea54834b2bca2af5fa89fd1e3ad"),
    ("logo", "41935bc1815004e25280343509e9df949298e5a3bd60c95fe293da07edbe96b9"),
    ("logo", "436ac3f24d0ef25181bcdbbcb41e97c74c4a75a8375271c8614df74d9611fd40"),
    ("logo", "ab1dd7c310762ccf41469c435902bf459e19ad0bfea8eeb97b251809b60cc1a7"),
    ("portrait", "12a022811e6a5ab56f32dbaa47275f451e9f847e9b5d2c68849d98e6c6f5bc6d"),
    ("portrait", "437033475e555827a1c4a402993f102cf3ab9eda8686119ce5cb8dab01d477d9"),
    ("portrait", "53b8036a62d89b84d81d1fcd0b000cc2852a513ed17177fa0e25658e5c14853e"),
    ("portrait", "978c9d3978e44dfb555f149266c415b1d95c20ddeff61feee46eda5a3029aed4"),
    ("portrait", "9f117a4818b18854f9908905ba91aba3a0be146bb30c3839ad59e584d0281cd3"),
    ("portrait", "ae29832eb268bed7c82b75d6cb82cb7e9e491ab2966ff8a1dfc4738bcecefb77"),
    ("portrait", "e800cd04427ec5e3e4aae6f17f67437d8845fca3b57f2f32da5de1a8e2bb16a9"),
    ("portrait", "f2b01f1fd32d0e968a301e340e20cb78b9e1819901d40a11047fea5679fb5a0a"),
    ("wide", "1334e30debb44b10988f716e24e15b091cdcb954d9ec9654be011217425559f4"),
    ("wide", "2423e7dc05b98870a9e4b9cd339035e30be6fe10c76cb3ac0a6f6d371ffb6f6d"),
    ("wide", "2bf7b1faba4f5d836f882903aa1dd09068ca4d15db8893d5b48a7670224f3263"),
    ("wide", "34b1b2498947c9e0101ab34bbd0c68d06fde9fe121bea91d16235ad31acb47c0"),
    ("wide", "76563cb90e71d9a291db3e0d9a6b58451afda0284ce44cff1326743a081b4058"),
    ("wide", "76e406dc3c8f36d78107a03ebf571c8be688e3ef40a4ffe18108c5b7e89456a5"),
    ("wide", "90e118e81004feea9fd04c4ad0cf8532ad6acf5739d3319f3835eb76db600829"),
    ("wide", "b1367f6c60cc779021812097c9b38510a1bae015a806b227f0d8b6b1341b02d3"),
];

/// Whether `bytes` is one of the launcher's own pictures for `kind` (this version's or an earlier one's).
fn is_our_builtin_art(kind: &str, testing: bool, bytes: &[u8]) -> bool {
    if steam_art_for(kind, testing) == bytes {
        return true;
    }
    let key = if kind == "icon" && testing { "icon_testing" } else { kind };
    in_table(OLD_STEAM_ART, key, bytes)
}

/// Whether the SHA-256 of `bytes` is listed for `key` in `table`.
fn in_table(table: &[(&str, &str)], key: &str, bytes: &[u8]) -> bool {
    let hash = format!("{:x}", Sha256::digest(bytes));
    table.iter().any(|(k, h)| *k == key && *h == hash)
}

// ---- putting it in place ------------------------------------------------------------------------------------------------------------

/// This window's icon. macOS has no window icon (the Dock shows the app's).
pub fn apply_window(app: &AppHandle, b: &Branding) {
    let Some(window) = app.get_webview_window("main") else { return };
    match tauri::image::Image::from_bytes(b.window_png()) {
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
        let ico = b.ico();
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
        let ico = b.ico();
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

/// A launcher start: the window, then both exes (an update or Set up may have written them with the default icon since). Then the
/// Steam art of shortcuts already added, so pictures of an older launcher become today's (nothing happens without Steam). Runs on the
/// cleanup thread after update::clean_start, so the `.old` of an update is gone before the launcher's own swap needs that name.
pub fn on_start(cfg: &Config) {
    let b = Branding::of(cfg);
    for result in [ensure_game_icon(&crate::paths::game_dir_of(cfg), &b), ensure_launcher_icon(&b), crate::steam::refresh_art(&b)] {
        if let Err(e) = result {
            eprintln!("branding: {e}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn own(testing: bool) -> Branding {
        Branding { testing, art: None }
    }

    fn themed(wide: &[u8], icon: &[u8]) -> Branding {
        let steam = HashMap::from([("wide", wide.to_vec()), ("icon", icon.to_vec())]);
        Branding { testing: false, art: Some(Arc::new(Art { window_png: Some(icon.to_vec()), ico: Some(b"ico".to_vec()), steam })) }
    }

    #[test]
    fn testing_has_its_own_icons() {
        let (stable, testing) = (own(false), own(true));
        assert_ne!(stable.steam_art("icon"), testing.steam_art("icon"));
        assert_eq!(stable.steam_art("logo"), testing.steam_art("logo"));
        assert_ne!(stable.ico(), testing.ico());
        assert_ne!(stable.window_png(), testing.window_png());
    }

    #[test]
    fn our_art_is_ours_and_a_players_own_art_is_not() {
        let b = own(false);
        for kind in ["wide", "portrait", "hero", "logo", "icon"] {
            assert!(b.is_our_steam_art(kind, steam_art_for(kind, false)), "{kind}");
            assert!(!b.is_our_steam_art(kind, b"a picture the player chose"));
        }
    }

    #[test]
    fn an_older_version_of_our_art_is_still_ours() {
        let real = format!("{:x}", Sha256::digest(b"old picture"));
        let table = [("wide", real.as_str())];
        assert!(in_table(&table, "wide", b"old picture"));
        assert!(!in_table(&table, "logo", b"old picture"));
        assert!(!in_table(&table, "wide", b"a random byte string"));
        assert!(OLD_STEAM_ART.iter().all(|(_, h)| h.len() == 64));
        assert!(!own(false).is_our_steam_art("wide", b"a random byte string"));
    }

    #[test]
    fn a_theme_overrides_what_it_has_and_its_pictures_count_as_ours() {
        let b = themed(b"theme wide", b"theme icon");
        assert_eq!(b.steam_art("wide"), b"theme wide");
        assert_eq!(b.steam_art("icon"), b"theme icon");
        assert_eq!(b.steam_art("hero"), steam_art_for("hero", false), "a picture the theme lacks stays the launcher's own");
        assert_eq!((b.ico(), b.window_png()), (b"ico".as_slice(), b"theme icon".as_slice()));
        assert_eq!(own(false).ico(), ico(false));
        assert!(b.is_our_steam_art("wide", b"theme wide"));
        assert!(b.is_our_steam_art("wide", steam_art_for("wide", false)), "the launcher's own picture stays replaceable");
        assert!(!b.is_our_steam_art("wide", b"a picture the player chose"));
    }
}
