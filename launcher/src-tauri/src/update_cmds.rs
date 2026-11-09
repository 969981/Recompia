// SPDX-License-Identifier: GPL-3.0-or-later
//! Update commands: the releases, one-click update, picking a build (update.rs holds the logic).

use serde::Serialize;
use std::path::Path;
use std::sync::Arc;
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};

use crate::config::{config_path, load_config};
use crate::paths::{exe_dir, game_dir_of, launcher_path};
use crate::relay_cmds::stop_relay;
use crate::setup_job::SetupState;
use crate::{play, rollback, saveimport, update};

/// The version updates compare with. None: a dev build, which takes none.
pub fn update_version(app: &AppHandle) -> Option<String> {
    #[allow(unused_mut)]
    let mut version = app.package_info().version.to_string();
    #[cfg(debug_assertions)]
    if let Ok(v) = std::env::var("PKP_LAUNCHER_UPDATE_CHECK_AS") {
        version = v; // debug builds only: pretend to be this version (dev builds otherwise skip the check)
    }
    (!update::is_dev_version(&version)).then_some(version)
}

/// The releases (update.rs load_releases) from this session's copy, the network or the cache file.
pub fn releases_of(app: &AppHandle, refresh: bool) -> Vec<update::GhRelease> {
    releases_for(app, refresh, update::Channel::parse(&load_config(app).channel))
}

/// Patch notes for the launcher and the update for this build, if there is one. Never fails: an empty list at worst.
#[tauri::command]
pub async fn get_releases(app: AppHandle, refresh: Option<bool>) -> update::Releases {
    tauri::async_runtime::spawn_blocking(move || update::view_here(&releases_of(&app, refresh.unwrap_or(false)), update_version(&app).as_deref())).await.unwrap_or_default()
}

/// A build the player picked (update.rs target_for_tag) instead of the newest newer one. `accepted`: the player confirmed the warning for
/// an older build (the dialogs ask; this side refuses without it).
pub struct Wanted {
    pub tag: String,
    pub channel: update::Channel,
    pub accepted: bool,
}

pub fn releases_for(app: &AppHandle, refresh: bool, channel: update::Channel) -> Vec<update::GhRelease> {
    let cache = config_path(app).map(|p| p.with_file_name("releases.json")).unwrap_or_default();
    update::load_releases(&app.state::<update::ReleaseStore>(), &cache, &app.package_info().version.to_string(), refresh, channel)
}

pub fn run_update(app: &AppHandle, st: &update::UpdateState, wanted: Option<Wanted>) -> Result<(), String> {
    let _claim = st.claim().ok_or("An update is already running.")?;
    // asked again right before files move: the download takes minutes
    let idle = || -> Result<(), String> {
        if app.state::<Arc<SetupState>>().progress.lock().unwrap().state == "running" {
            return Err("Setup is running. Pause it first, then update.".into());
        }
        if app.state::<play::PlayState>().0.lock().unwrap().state == "running" || saveimport::game_running() {
            return Err("The game is running. Close it first, then update.".into());
        }
        Ok(())
    };
    idle()?;
    let version = update_version(app).ok_or("This build does not update itself.")?;
    // the newest newer build, or the one the player picked
    let (target, older) = match &wanted {
        None => (update::target_here(&releases_of(app, false), &version).ok_or("No update is available right now.")?, false),
        Some(w) => {
            let t = update::target_for_tag_here(&releases_for(app, false, w.channel), &w.tag).ok_or("That build cannot be installed on this system.")?;
            (t, update::check_pick(&version, &w.tag, w.accepted)?)
        }
    };
    let paths = update::Paths {
        install: exe_dir(),
        launcher: launcher_path().ok_or("Could not find the launcher's own file.")?,
        game_dir: game_dir_of(&load_config(app)),
    };
    let mut report = |phase: &str, done: u64, total: u64| {
        let _ = app.emit("update-progress", UpdateProgress { phase: phase.to_string(), done, total });
    };
    // an older build: the player's data is copied first (rollback.rs), after the download and right before the files move; if the copy
    // cannot be made nothing is replaced
    let config_dir = config_path(app).ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default();
    let ready = || -> Result<(), String> {
        idle()?;
        if older {
            rollback::backup(&paths.game_dir, &config_dir, &target.tag)?;
        }
        Ok(())
    };
    let session = update::install(&target, &paths, st, &mut report, &ready)?;
    report("restart", 0, 1);
    stop_relay(app);
    if let Err(e) = update::relaunch(&paths.launcher, &paths.install) {
        return Err(match session.rollback() {
            Ok(()) => format!("The new launcher did not start ({}). The update was undone.", e),
            Err(left) => format!("The new launcher did not start ({}). {}", e, left),
        });
    }
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.hide();
    }
    session.finish();
    app.exit(0);
    Ok(())
}

#[derive(Serialize, Clone)]
pub struct UpdateProgress {
    /// "download", "verify", "extract", "apply" or "restart"
    pub phase: String,
    pub done: u64,
    pub total: u64,
}

/// One-click update (update.rs). Emits "update-progress"; on success the launcher restarts and the page hears nothing more.
/// The Err of a cancel is update::CANCELLED.
#[tauri::command]
pub async fn install_update(app: AppHandle, state: State<'_, Arc<update::UpdateState>>) -> Result<(), String> {
    let st = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || run_update(&app, &st, None)).await.map_err(|e| e.to_string())?
}

/// The builds a channel offers, newest first, with their notes and what going to each one means (update.rs builds). Works for Testing
/// before the channel is saved, so the join question can list them. Empty for a build that takes no updates.
#[tauri::command]
pub async fn list_builds(app: AppHandle, channel: String, refresh: Option<bool>) -> Vec<update::Build> {
    let Some(version) = update_version(&app) else { return Vec::new() };
    tauri::async_runtime::spawn_blocking(move || update::builds_here(&releases_for(&app, refresh.unwrap_or(false), update::Channel::parse(&channel)), &version)).await.unwrap_or_default()
}

/// Installs the build the player picked (an older one only with `accepted`); the same steps and restart as the update.
#[tauri::command]
pub async fn install_version(app: AppHandle, state: State<'_, Arc<update::UpdateState>>, tag: String, channel: String, accepted: bool) -> Result<(), String> {
    let st = state.inner().clone();
    let wanted = Wanted { tag, channel: update::Channel::parse(&channel), accepted };
    tauri::async_runtime::spawn_blocking(move || run_update(&app, &st, Some(wanted))).await.map_err(|e| e.to_string())?
}

/// Stops a running update while it is still downloading or unpacking; the install stays as it was.
#[tauri::command]
pub fn cancel_update(state: State<Arc<update::UpdateState>>) {
    state.cancel();
}
