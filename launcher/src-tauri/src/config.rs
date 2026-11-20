// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's own options (launcher.json in the user's app config folder).

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use tauri::{AppHandle, Manager};

use crate::settings;

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    pub dump_dir: String,
    /// Folder with pkp.exe. Empty: the folder the launcher is in.
    pub game_dir: String,
    pub minimize_on_play: bool,
    /// Show "Playing Recompia" on the player's Discord profile while the game runs (presence.rs). Off unless the player switches it on.
    pub discord_presence: bool,
    /// The first Play after online presence came asked once whether to switch it on (home.js); never asked again. The switch itself is
    /// pkp.ini [pkp] online_presence (online.rs), shared with the game's own Settings screen.
    pub online_asked: bool,
    /// Cloud Islands with an online account (experimental; Settings > Game > Online). On: the game is pointed at the Cloud Islands
    /// server, Home has the account chip and the sign-in screen, and the account gateway runs with the game (cloud_account.rs,
    /// cloud_signin.rs). Off unless the player switches it on; off, the launcher is as it was without them. Switching it off keeps
    /// the sign-in (the device folder).
    pub cloud_islands: bool,
    /// The player pressed Skip on the sign-in screen the launcher starts on while this device is signed out (pages/welcome.js,
    /// cloud-signin.js): it is not opened by itself again until the next sign-in or sign-out clears this.
    pub signin_skipped: bool,
    /// The name online.rs sends; empty (the default): anonymous. The profile's nickname is never sent.
    pub online_name: String,
    /// Random, made the first time online presence starts; with the profile uid it makes the online ID (online.rs token_of). Only
    /// "Reset my online ID" changes it, by emptying it.
    pub online_salt: String,
    /// Set up with fewer cores at idle priority (preflight.rs jobs_for)
    pub gentle_build: bool,
    /// A new setup keeps its translated shaders as ~99,000 loose files (the old storage) instead of the shader archive. Only read when
    /// a setup starts with no translated shader yet (shaders.rs Job::compact). Loose storage is on its way out.
    pub loose_shaders: bool,
    /// Where the window was last time, on a desktop screen (logical pixels); none: the default size, centred
    pub window: Option<WindowPlace>,
    /// Which releases the launcher offers: "stable" (the default) or "experimental" (the prereleases, each a feature test). releases.rs
    /// Channel. The retired "testing" reads as "stable" (`migrate`).
    pub channel: String,
    /// The version the player picked next to Play (a release tag, "v0.15.1"). On Stable, empty is Latest: the Update button offers the
    /// newest release; a picked version is kept and no update is offered. On Experimental it is the prerelease the player picked; empty
    /// until they pick one, and Experimental never offers an update either way. Written by the page, before the install it starts; it
    /// counts only while it names the running version (update_cmds.rs follows_latest), so an install that never finished leaves Latest.
    pub picked_version: String,
    /// Once per launcher start, update the installed mods from the mod hub (mods_update.rs). Off unless the player switches it on.
    pub mods_auto_update: bool,
    /// The player unlocked the Community Achievements mod (mods_lock.rs): it may be switched off or removed, and the launcher no longer
    /// installs or switches it on by itself. Locked unless the player unlocks it.
    pub achievements_unlocked: bool,
    /// The player unlocked the mod's sharing setting (mods_lock.rs): it may be switched off. Locked unless the player unlocks it.
    pub achievements_sharing_unlocked: bool,
    /// The install folder whose first start under the launcher's new file name already ran (rename_migrate.rs); empty: none yet. A folder,
    /// not a yes/no, because every install on the machine shares this file.
    pub renamed_in: String,
    // Retired keys an older launcher.json may still hold are skipped (serde ignores unknown fields): `logo` and `icon` (the logo and
    // icon choice, removed with the Classic look).
    /// The player's own theme (themes.rs): `<pack folder>/<theme id>`. Empty: one of the launcher's own looks (the page keeps which in
    /// localStorage). Rust reads it for the icon and the Steam pictures a theme changes; the page for the colours and pictures.
    pub theme: String,
    /// Which game the launcher shows (src/games.js): "recompia" or "coop". Empty (an older launcher.json): "recompia". Only the pages
    /// read it; both games are the same install, build and shaders.
    pub game: String,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
pub struct WindowPlace {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

pub fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

/// The game folder the saved settings name (the launcher's own folder when none).
pub fn game_dir(app: &AppHandle) -> PathBuf {
    crate::paths::game_dir_of(&load_config(app))
}

pub fn load_config(app: &AppHandle) -> Config {
    let cfg: Config = config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default();
    migrate(cfg)
}

/// What an older launcher.json means today. The Testing channel is retired: its players are on Stable with Latest (the newest release,
/// updated as before), and pick a prerelease themselves if they want Experimental. The file is rewritten at the next save.
pub fn migrate(mut cfg: Config) -> Config {
    if cfg.channel.trim().eq_ignore_ascii_case("testing") {
        cfg.channel = "stable".into();   // releases.rs Channel::Stable (config.rs is also built into the examples, without releases.rs)
        cfg.picked_version.clear();
    }
    cfg
}

#[tauri::command]
pub fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
pub fn save_config(app: AppHandle, mut config: Config) -> Result<(), String> {
    // a page sends the copy it read when it opened: the chosen theme is set only by set_theme (themes_cmds.rs), so an older copy never undoes it
    config.theme = load_config(&app).theme;
    store_config(&app, &config)
}

/// Writes launcher.json (the page's save_config, and a command that changes one option of its own).
pub fn store_config(app: &AppHandle, config: &Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(app)?, &text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

/// The install's online salt, made and saved the first time it is needed. None: it could not be saved, so no online ID is used (an ID
/// that changed every run would split one player into many).
pub fn online_salt(app: &AppHandle) -> Option<String> {
    let mut cfg = load_config(app);
    if cfg.online_salt.is_empty() {
        let mut bytes = [0u8; 16];
        pokopia_relay::random::os_random(&mut bytes).ok()?;
        cfg.online_salt = bytes.iter().map(|b| format!("{:02x}", b)).collect();
        save_config(app.clone(), cfg.clone()).ok()?;
    }
    Some(cfg.online_salt)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_old_launcher_json_with_retired_keys_still_loads() {
        let cfg: Config = serde_json::from_str(r#"{"channel":"testing","logo":"classic","icon":"classic"}"#).unwrap();
        assert_eq!(cfg.theme, "", "a launcher.json from before themes has none chosen");
        assert_eq!(cfg.channel, "testing");
        assert_eq!(cfg.picked_version, "", "no version picked: Latest");
        assert!(!cfg.signin_skipped, "a launcher.json from before the sign-in offer has not skipped it");
        assert!(!cfg.cloud_islands, "Cloud Islands are off unless the player switches them on");
    }

    #[test]
    fn the_testing_channel_becomes_stable_latest() {
        let read = |t: &str| migrate(serde_json::from_str::<Config>(t).unwrap());
        for t in [r#"{"channel":"testing"}"#, r#"{"channel":" Testing "}"#, r#"{"channel":"testing","picked_version":"v0.16.0-beta.2"}"#] {
            let cfg = read(t);
            assert_eq!((cfg.channel.as_str(), cfg.picked_version.as_str()), ("stable", ""), "{t}");
        }
        let cfg = read(r#"{"channel":"experimental","picked_version":"v0.16.0-beta.2"}"#);
        assert_eq!((cfg.channel.as_str(), cfg.picked_version.as_str()), ("experimental", "v0.16.0-beta.2"));
        let cfg = read(r#"{"channel":"stable","picked_version":"v0.15.0"}"#);
        assert_eq!((cfg.channel.as_str(), cfg.picked_version.as_str()), ("stable", "v0.15.0"), "a pinned Stable version is kept");
        assert_eq!(read("{}").channel, "", "no channel: Stable, as before");
    }
}
