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
use crate::{rollback, setuplog, update};

/// The version updates compare with. None: a dev build, which takes none, or an install RecompLauncher keeps (managed.rs), which
/// takes its updates from that launcher only.
pub fn update_version(app: &AppHandle) -> Option<String> {
    if crate::managed::by_recomplauncher() {
        return None;
    }
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

/// The version the Update button compares with: None on a channel that offers no updates (Experimental, releases.rs
/// Channel::offers_updates) and on Stable with a version the player picked (config.rs `picked_version`): only Latest updates.
fn update_current(app: &AppHandle) -> Option<String> {
    let cfg = load_config(app);
    update_version(app).filter(|v| follows_latest(&cfg.channel, &cfg.picked_version, v))
}

/// Whether the player follows the newest release: Stable with Latest picked. A picked version counts only while it is the version
/// running (`running`): a pick whose install never finished (the launcher was closed or crashed during it), or one an older launcher
/// updated away from (it knows no picks), is Latest again, so a player is never left on a pin they do not have.
pub fn follows_latest(channel: &str, picked: &str, running: &str) -> bool {
    let bare = |v: &str| v.trim().trim_start_matches(['v', 'V']).to_string();
    Channel::parse(channel).offers_updates() && (picked.trim().is_empty() || bare(picked) != bare(running))
}

#[cfg(test)]
mod tests {
    use super::follows_latest;

    #[test]
    fn only_stable_with_latest_or_a_pick_it_is_not_on_takes_updates() {
        assert!(follows_latest("stable", "", "0.15.1"), "Latest");
        assert!(follows_latest("", "", "0.15.1"), "no channel: Stable, Latest");
        assert!(follows_latest("testing", "", "0.16.0-beta.1"), "the retired Testing: Stable, Latest");
        assert!(!follows_latest("stable", "v0.15.0", "0.15.0"), "a picked version stays");
        assert!(!follows_latest("stable", " V0.15.0 ", "0.15.0"));
        assert!(follows_latest("stable", "v0.15.0", "0.15.1"), "a pick that is not installed (install cut off, or updated away) is Latest");
        assert!(!follows_latest("experimental", "", "0.16.0-beta.1"), "Experimental never updates");
        assert!(!follows_latest("experimental", "v0.16.0-beta.1", "0.15.1"), "not even when its pick is not installed");
    }
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

/// An update, with its phases and its error in setup.log: otherwise the error lives only in the dialog, and a player's report of it
/// comes with a setup.log that ends at the last update that worked.
pub fn run_update(app: &AppHandle, st: &update::UpdateState, wanted: Option<Wanted>) -> Result<(), String> {
    let log = setuplog::SetupLog::open(&game_dir(app));
    let r = update_steps(app, st, wanted, &log);
    if let Err(e) = &r {
        log.line(&format!("=== Update stopped: {e}"));
    }
    r
}

fn update_steps(app: &AppHandle, st: &update::UpdateState, wanted: Option<Wanted>, log: &setuplog::SetupLog) -> Result<(), String> {
    let _claim = st.claim().ok_or("An update is already running.")?;
    // the launcher restarts to finish its update, and a Zonith download would be cut off
    if crate::zonith_cmds::job_running() {
        return Err("Zonith is downloading. Let it finish or cancel it first.".into());
    }
    // Set up builds in the game folder the update prepares in. The game may run: the update downloads and prepares meanwhile, and
    // waits for it to close before files move (`busy`, asked until then)
    const SETUP_RUNS: &str = "Setup is running. Pause it first, then update.";
    ensure_idle(app, &[Need::Setup(SETUP_RUNS)])?;
    let busy = || ensure_idle(app, &[Need::Setup(SETUP_RUNS), Need::Game("The game is running. Close it first, then update.")]).err();
    let version = update_version(app).ok_or("This build does not update itself.")?;
    // the newest newer build, or the one the player picked
    let (target, older) = match &wanted {
        None => {
            update_current(app).ok_or("Updates only come on Stable with Latest picked. To move to another version, pick it next to Play.")?;
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
    log.line(&format!("=== Update from v{version} to {}: started in {}", target.tag, paths.install.display()));
    // one line per phase; the game build's own steps go to the log through GameStep
    let mut phase = String::new();
    let mut report = |p: update::Progress| {
        if p.phase != phase {
            phase.clone_from(&p.phase);
            log.line(&format!("update: {phase}"));
        }
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
    // the new launcher: where the running one was, or its own name beside it after an update across the release rename
    if let Err(e) = update::relaunch(session.launcher(), &paths.install) {
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

/// The builds a channel's version picker offers (releases.rs pickable: Stable the releases, Experimental every prerelease), newest
/// first, with their notes and what going to each one means (releases.rs builds). Works for a channel that is not saved yet, so
/// joining Experimental can list its builds first. From this session's copy of the list unless `refresh`. Empty for a build that
/// takes no updates.
#[tauri::command]
pub async fn list_builds(app: AppHandle, channel: String, refresh: Option<bool>) -> Vec<releases::Build> {
    let Some(version) = update_version(&app) else { return Vec::new() };
    tauri::async_runtime::spawn_blocking(move || {
        let channel = Channel::parse(&channel);
        releases::builds_here(&releases::pickable(releases_for(&app, refresh.unwrap_or(false), channel), channel), &version)
    }).await.unwrap_or_default()
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
