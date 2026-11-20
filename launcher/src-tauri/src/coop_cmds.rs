// SPDX-License-Identifier: GPL-3.0-or-later
//! Co-op commands (coop.rs holds the profiles, coop_save.rs the folder and who hosts, coop_launch.rs what the game is told, coop_diag.rs
//! what the game's log says about a start that found nobody).

use serde::Serialize;
use std::path::{Path, PathBuf};
use tauri::AppHandle;

use crate::config::game_dir;
use crate::coop::{self, Profile};
use crate::coop_launch::{self, SplitOptions};
use crate::coop_save::{self, HostChoice, StartAs};
use crate::error::{self, CmdError};
use crate::guard::{ensure_idle, Need};
use crate::states::States;
use crate::{coop_code, launch, play, saveimport};

/// A co-op save as the page shows it: the profile, who would host if it started now, and where it lives.
#[derive(Serialize)]
pub struct CoopView {
    pub profile: Profile,
    pub host: HostChoice,
    pub folder: String,
    /// This PC plays the creator, who changes the member list and sends online invites.
    pub creator: bool,
    /// Unix seconds of this PC's copy (window 1 in split screen), 0 before it has a save.
    pub saved_at: u64,
    /// Unix seconds the save was made or joined on this PC (its marker's time); 0 when unknown.
    pub created: u64,
}

fn file_time(path: &Path) -> u64 {
    std::fs::metadata(path).and_then(|m| m.modified()).ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs())
}

fn view(root: &Path, p: Profile) -> CoopView {
    let dir = coop_save::folder_of(root, &p.id);
    let host = coop_save::host_of(root, &p);
    CoopView { folder: crate::dump::fwd(&dir), creator: p.is_creator(), saved_at: host.own.saved_at, created: file_time(&dir.join(coop_save::MARKER)), profile: p, host }
}

fn views(root: &Path) -> Vec<CoopView> {
    coop::list(root).into_iter().map(|p| view(root, p)).collect()
}

/// A change to a co-op save waits for the game: it reads coop.json while it runs.
fn guarded_root(app: &AppHandle) -> Result<PathBuf, String> {
    ensure_idle(app, &[Need::Game("The game is running. Close it first, then change the co-op save.")])?;
    Ok(coop::root_of(&game_dir(app)))
}

#[tauri::command]
pub fn coop_list(app: AppHandle) -> Vec<CoopView> {
    views(&coop::root_of(&game_dir(&app)))
}

#[tauri::command]
pub fn coop_create(app: AppHandle, name: String, my_name: String, relay: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    coop::create(&root, &name, &my_name, &relay)?;
    Ok(views(&root))
}

#[tauri::command]
pub fn coop_create_split(app: AppHandle, name: String, players: Vec<String>) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    coop::create_split(&root, &name, &players)?;
    Ok(views(&root))
}

#[tauri::command]
pub fn coop_add_member(app: AppHandle, id: String, name: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    coop::add_member(&root, &id, &name)?;
    Ok(views(&root))
}

#[tauri::command]
pub fn coop_remove_member(app: AppHandle, id: String, member: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    coop::remove_member(&root, &id, &member)?;
    Ok(views(&root))
}

/// An invite: the long text, and its short code when relay `relay` (the one joiners' launchers ask) keeps one (coop_code.rs). Both carry
/// the private room, so they go to the members only.
#[derive(Serialize)]
pub struct InviteCodes {
    pub long: String,
    pub short: Option<String>,
}

/// On a worker thread: the short code waits for the relay.
#[tauri::command]
pub async fn coop_invite(app: AppHandle, id: String, relay: String) -> Result<InviteCodes, String> {
    let root = coop::root_of(&game_dir(&app));
    tauri::async_runtime::spawn_blocking(move || {
        let p = coop::get(&root, &id)?;
        let long = coop::invite(&root, &id)?;
        let short = coop_code::shorten(&p, &relay, &long);
        Ok(InviteCodes { long, short })
    })
    .await
    .map_err(|e| e.to_string())?
}

/// The long invite for what the player typed under Join: a short code is looked up on `relay`, other text comes back as it is.
/// "No invite has this code" is tagged (error::INVITE_NOT_FOUND): the page keeps that answer for the text, and asks again after any other.
#[tauri::command]
pub async fn coop_resolve_invite(text: String, relay: String) -> Result<String, CmdError> {
    tauri::async_runtime::spawn_blocking(move || coop_code::resolve(&relay, &text))
        .await
        .map_err(|e| e.to_string())?
        .map_err(|m| CmdError::coded_if(m, coop_code::NOT_FOUND, error::INVITE_NOT_FOUND))
}

/// What an invite holds for the page: the save it is for and its players (creator first), so the page can ask which one you are, or
/// say it updates a save this PC has. None: not an invite.
#[derive(Serialize)]
pub struct InviteView {
    pub id: String,
    pub members: Vec<coop::Member>,
}

#[tauri::command]
pub fn coop_invite_members(invite: String) -> Option<InviteView> {
    Some(InviteView { id: coop::invite_save_id(&invite)?, members: coop::invite_members(&invite)? })
}

/// Joins from an invite as `member`, a player the creator added.
#[tauri::command]
pub fn coop_accept(app: AppHandle, invite: String, member: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    coop::accept(&root, &invite, &member)?;
    Ok(views(&root))
}

/// Deletes co-op save `id` from this PC: its folder (this PC's copy of the world) and its own mod settings. The other players keep theirs.
/// On the creator's PC its short codes are dropped from relay `relay` (the one coop_invite was given) too, when the save plays there
/// (coop_code::forget, in the background: the page does not wait on the relay); a member's delete leaves the code for the others.
#[tauri::command]
pub fn coop_delete(app: AppHandle, id: String, relay: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    let layer = crate::mods_scope::Scope::Coop(id.clone()).layer_dir(&game_dir(&app)).ok().flatten();
    let code = coop::get(&root, &id).ok().filter(|p| p.is_creator()).and_then(|p| Some((coop::invite(&root, &id).ok()?, p)));
    coop::delete(&root, &id)?;
    if let Some(dir) = layer {
        let _ = std::fs::remove_dir_all(dir);   // the group's own mod choices: only this group's account uses that layer
    }
    if let Some((invite, p)) = code {
        std::thread::spawn(move || coop_code::forget(&p, &relay, &invite));
    }
    Ok(views(&root))
}

/// Why the co-op start the page made at `since` (unix seconds) found no host, from the game's log; None when the log says nothing of it.
#[tauri::command]
pub fn coop_last_run(app: AppHandle, since: u64) -> Option<crate::coop_diag::Finding> {
    crate::coop_diag::last_run(&game_dir(&app), since)
}

/// Online saves start this PC's member in the private room, with role `start_as` (auto, host or visit). Split saves start every
/// member in its own window and use the automatic host choice.
#[tauri::command]
pub fn coop_play(app: AppHandle, id: String, start_as: Option<StartAs>, split: Option<SplitOptions>) -> Result<(), String> {
    let updates = app.updates();
    let _swap = updates.hold_swap();
    let (dir, cfg) = launch::prepare(&app)?;
    let (singleplayer, _) = saveimport::locate(&dir);
    let root = coop::root_beside(&singleplayer);
    let p = coop::get(&root, &id)?;
    if p.split {
        ensure_idle(&app, &[Need::Game("The game is already running.")])?;
    }
    // the group's own mod set is the layer of its account (mods_scope.rs); the game picks it from NXRT_ACCOUNT_UID
    crate::mods::ensure_points_built(&dir, &crate::mods::Scope::Coop(p.id.clone()))?;
    let save = coop_save::folder_of(&root, &p.id);
    let (go, _) = if p.split {
        coop_save::ensure_playable(&save, &p.id, &singleplayer)?;
        coop_save::ensure_split_folders(&root, &p)?;
        let start = coop_launch::build_split(&p, &root, &singleplayer, &split.unwrap_or_default())?;
        coop_launch::prepare_windows(&dir, p.members.len())?;
        start
    } else {
        coop_launch::build(&p, &save, &singleplayer, start_as.unwrap_or_default())?
    };
    play::start(&app, &dir, &cfg, false, Some(go))
}

#[cfg(test)]
mod tests {
    use super::*;

    // the page (src/pages/coop.js) reads these keys and values: a rename here must be made there too
    #[test]
    fn the_view_has_the_shape_the_page_reads() {
        let root = std::env::temp_dir().join(format!("pkp-coopview-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let p = coop::create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        let v = serde_json::to_value(view(&root, p)).unwrap();
        for k in ["profile", "host", "folder", "creator", "saved_at", "created"] {
            assert!(v.get(k).is_some(), "{}", k);
        }
        for k in ["host", "role", "own", "peers_known"] {
            assert!(v["host"].get(k).is_some(), "host.{}", k);
        }
        assert_eq!(v["host"]["role"], "host");
        assert_eq!(v["creator"], true);
        assert_eq!(v["saved_at"], 0);
        for k in ["id", "name", "members", "me", "room_code", "relay", "split"] {
            assert!(v["profile"].get(k).is_some(), "profile.{}", k);
        }
        assert_eq!(v["profile"]["split"], false);
        let split = coop::create_split(&root, "Local world", &["Alpha".into(), "Beta".into()]).unwrap();
        coop_save::ensure_split_folders(&root, &split).unwrap();
        let second = &split.members[1].id;
        let copy = coop_save::split_folder_of(&root, &split.id, second);
        std::fs::write(copy.join(coop_save::STAMP), r#"{"seq":2,"saved_at":42}"#).unwrap();
        let v = serde_json::to_value(view(&root, split.clone())).unwrap();
        assert_eq!(v["profile"]["split"], true);
        assert_eq!(v["profile"]["relay"], "");
        assert_eq!(v["profile"]["members"].as_array().unwrap().len(), 2);
        assert_eq!(v["profile"]["me"], split.me.as_str());
        assert_eq!(v["host"]["host"], split.members[0].id.as_str(), "player 1 hosts a split save, a newer copy or not");
        assert_eq!(v["host"]["role"], "host");
        assert_eq!(v["creator"], true);
        let f = serde_json::to_value(crate::coop_diag::Finding { mismatch: crate::coop_diag::Mismatch::Mods, own_mods: String::new() }).unwrap();
        assert_eq!(f["mismatch"], "mods");
    }
}
