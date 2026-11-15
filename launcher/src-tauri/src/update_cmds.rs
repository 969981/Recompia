// SPDX-License-Identifier: GPL-3.0-or-later
//! Update commands: the releases, one-click update, picking a build (releases.rs and update.rs hold the logic).

use std::path::Path;
use tauri::Emitter;
use tauri::{AppHandle, Manager};

use crate::states::{UpdateArg, States};
use crate::config::{config_path, game_dir, load_config};
use crate::update_game::GameStep;
use crate::error::{self, CmdError};
use crate::guard::{ensure_idle, Need};
use crate::paths::{exe_dir, launcher_path};
use crate::relay_cmds::stop_relay;
use crate::releases::{self, Channel};
use crate::{rollback, update};

/// The version updates compare with. None: a dev build, which takes none.
pub fn update_version(app: &AppHandle) -> Option<String> {
    #[allow(unused_mut)]
    let mut version = app.package_info().version.to_string();
    #[cfg(debug_assertions)]
    if let Ok(v) = std::env::var("PKP_LAUNCHER_UPDATE_CHECK_AS") {
        version = v; // debug builds only: pretend to be this version (dev builds otherwise skip the check)
    }
    (!releases::is_dev_version(&version)).then_some(version)
}

/// The releases (releases.rs load_releases) from this session's copy, the network or the cache file.
pub fn releases_of(app: &AppHandle, refresh: bool) -> Vec<releases::GhRelease> {
    releases_for(app, refresh, Channel::parse(&load_config(app).channel))
}

/// The version the Update button compares with: None on a channel that offers no updates (Testing, releases.rs Channel::offers_updates).
fn update_current(app: &AppHandle) -> Option<String> {
    update_version(app).filter(|_| Channel::parse(&load_config(app).channel).offers_updates())
}

/// Patch notes for the launcher and the update for this build, if there is one. Never fails: an empty list at worst.
#[tauri::command]
pub async fn get_releases(app: AppHandle, refresh: Option<bool>) -> releases::Releases {
    tauri::async_runtime::spawn_blocking(move || releases::view_here(&releases_of(&app, refresh.unwrap_or(false)), update_current(&app).as_deref())).await.unwrap_or_default()
}

/// A build the player picked (releases.rs target_for_tag) instead of the newest newer one. `accepted`: the player confirmed the warning for
/// an older build (the dialogs ask; this side refuses without it).
pub struct Wanted {
    pub tag: String,
    pub channel: Channel,
    pub accepted: bool,
}

pub fn releases_for(app: &AppHandle, refresh: bool, channel: Channel) -> Vec<releases::GhRelease> {
    let cache = config_path(app).map(|p| p.with_file_name("releases.json")).unwrap_or_default();
    releases::load_releases(&app.releases(), &cache, &app.package_info().version.to_string(), refresh, channel)
}

pub fn run_update(app: &AppHandle, st: &update::UpdateState, wanted: Option<Wanted>) -> Result<(), String> {
    let _claim = st.claim().ok_or("An update is already running.")?;
    // Set up builds in the game folder the update prepares in. The game may run: the update downloads and prepares meanwhile, and
    // waits for it to close before files move (`busy`, asked until then)
    const SETUP_RUNS: &str = "Setup is running. Pause it first, then update.";
    ensure_idle(app, &[Need::Setup(SETUP_RUNS)])?;
    let busy = || ensure_idle(app, &[Need::Setup(SETUP_RUNS), Need::Game("The game is running. Close it first, then update.")]).err();
    let version = update_version(app).ok_or("This build does not update itself.")?;
    // the newest newer build, or the one the player picked
    let (target, older) = match &wanted {
        None => {
            update_current(app).ok_or("The Testing channel has no automatic update. Pick a build in Settings > About > Choose a version.")?;
            (releases::target_here(&releases_of(app, false), &version).ok_or("No update is available right now.")?, false)
        }
        Some(w) => {
            let t = releases::target_for_tag_here(&releases_for(app, false, w.channel), &w.tag).ok_or("That build cannot be installed on this system.")?;
            (t, releases::check_pick(&version, &w.tag, w.accepted)?)
        }
    };
    let paths = update::Paths {
        install: exe_dir(),
        launcher: launcher_path().ok_or("Could not find the launcher's own file.")?,
        game_dir: game_dir(app),
    };
    let mut report = |p: update::Progress| {
        let _ = app.emit("update-progress", p);
    };
    // an older build: the player's data is copied first (rollback.rs), after the download and right before the files move; if the copy
    // cannot be made nothing is replaced
    let config_dir = config_path(app).ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default();
    let ready = || -> Result<(), String> {
        if older {
            rollback::backup(&paths.game_dir, &config_dir, &target.tag)?;
        }
        Ok(())
    };
    let code = GameStep::new(&load_config(app), &target.tag);
    let session = update::install(&target, &paths, st, &mut report, &update::Steps { busy: &busy, ready: &ready, code: &code })?;
    report(update::Progress::of("restart", "", 0, 1));
    stop_relay(app);
    if let Err(e) = update::relaunch(&paths.launcher, &paths.install) {
        return Err(match session.rollback(&code) {
            Ok(()) => format!("The new launcher did not start ({}). The update was undone.", e),
            Err(left) => format!("The new launcher did not start ({}). {}", e, left),
        });
    }
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.hide();
    }
    session.finish(&code);
    app.exit(0);
    Ok(())
}

/// One-click update (update.rs). Emits "update-progress" (update::Progress); on success the launcher restarts and the page hears
/// nothing more.
/// The Err of a cancel has the code error::UPDATE_CANCELLED.
#[tauri::command]
pub async fn install_update(app: AppHandle, state: UpdateArg<'_>) -> Result<(), CmdError> {
    let st = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || run_update(&app, &st, None)).await.map_err(|e| e.to_string())?.map_err(update_error)
}

/// A cancelled update carries the code the page matches; every other failure is plain.
fn update_error(message: String) -> CmdError {
    CmdError::coded_if(message, update::CANCELLED, error::UPDATE_CANCELLED)
}

/// The builds a channel offers, newest first, with their notes and what going to each one means (releases.rs builds). Works for Testing
/// before the channel is saved, so the join question can list them. Empty for a build that takes no updates.
#[tauri::command]
pub async fn list_builds(app: AppHandle, channel: String, refresh: Option<bool>) -> Vec<releases::Build> {
    let Some(version) = update_version(&app) else { return Vec::new() };
    tauri::async_runtime::spawn_blocking(move || releases::builds_here(&releases_for(&app, refresh.unwrap_or(false), Channel::parse(&channel)), &version)).await.unwrap_or_default()
}

/// Installs the build the player picked (an older one only with `accepted`); the same steps and restart as the update.
#[tauri::command]
pub async fn install_version(app: AppHandle, state: UpdateArg<'_>, tag: String, channel: String, accepted: bool) -> Result<(), CmdError> {
    let st = state.inner().clone();
    let wanted = Wanted { tag, channel: Channel::parse(&channel), accepted };
    tauri::async_runtime::spawn_blocking(move || run_update(&app, &st, Some(wanted))).await.map_err(|e| e.to_string())?.map_err(update_error)
}

/// Stops a running update until its files start to move (downloading, unpacking, preparing the game, waiting for it to close); the
/// install stays as it was, and the game code prepared so far is kept for the next update.
#[tauri::command]
pub fn cancel_update(state: UpdateArg) {
    state.cancel();
}
