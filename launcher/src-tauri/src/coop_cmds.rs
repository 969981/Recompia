// SPDX-License-Identifier: GPL-3.0-or-later
//! Co-op commands (coop.rs holds the profiles, coop_save.rs the folder and who hosts, coop_launch.rs what the game is told, coop_diag.rs
//! what the game's log says about a start that found nobody).

use serde::Serialize;
use std::path::{Path, PathBuf};
use tauri::AppHandle;

use crate::config::game_dir;
use crate::coop::{self, Profile};
use crate::coop_save::{self, HostChoice, StartAs};
use crate::guard::{ensure_idle, Need};
use crate::states::States;
use crate::{coop_launch, launch, play, saveimport};

/// A co-op save as the page shows it: the profile, who would host if it started now, and where it lives.
#[derive(Serialize)]
pub struct CoopView {
    pub profile: Profile,
    pub host: HostChoice,
    pub folder: String,
    /// This PC plays the creator, who adds and removes players and sends the invite.
    pub creator: bool,
    /// Unix seconds of this PC's copy of the world (its stamp, else its newest file); 0: never played here.
    pub saved_at: u64,
    /// Unix seconds the save was made or joined on this PC (its marker's time); 0 when unknown.
    pub created: u64,
}

fn file_time(path: &Path) -> u64 {
    std::fs::metadata(path).and_then(|m| m.modified()).ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs())
}

fn view(root: &Path, p: Profile) -> CoopView {
    let dir = coop_save::folder_of(root, &p.id);
    let host = coop_save::choose_host(&p.member_ids(), &p.me, coop_save::own_stamp(&dir), &coop_save::peer_stamps(&dir));
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

/// The invite text: it carries the private room's code, so it goes to the members only.
#[tauri::command]
pub fn coop_invite(app: AppHandle, id: String) -> Result<String, String> {
    coop::invite(&coop::root_of(&game_dir(&app)), &id)
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
#[tauri::command]
pub fn coop_delete(app: AppHandle, id: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    let layer = crate::mods_scope::Scope::Coop(id.clone()).account(&game_dir(&app)).ok().flatten();
    coop::delete(&root, &id)?;
    if let Some(key) = layer {
        let _ = std::fs::remove_dir_all(game_dir(&app).join("config").join("accounts").join(key));   // the group's mod layer (mods_scope.rs)
    }
    Ok(views(&root))
}

/// Why the co-op start the page made at `since` (unix seconds) found no host, from the game's log; None when the log says nothing of it.
#[tauri::command]
pub fn coop_last_run(app: AppHandle, since: u64) -> Option<crate::coop_diag::Finding> {
    crate::coop_diag::last_run(&game_dir(&app), since)
}

/// Starts the game as a member of co-op save `id`: its own folder as the save folder, the private room, and who hosts. `start_as`:
/// "auto" (the default), "host" or "visit", for this start only.
#[tauri::command]
pub fn coop_play(app: AppHandle, id: String, start_as: Option<StartAs>) -> Result<(), String> {
    let updates = app.updates();
    let _swap = updates.hold_swap();
    let (dir, cfg) = launch::prepare(&app)?;
    let root = coop::root_of(&dir);
    let p = coop::get(&root, &id)?;
    // the group's own mod set is the layer of its account (mods_scope.rs); the game picks it from NXRT_ACCOUNT_UID
    crate::mods::ensure_points_built(&dir, &crate::mods::Scope::Coop(p.id.clone()))?;
    let (go, _) = coop_launch::build(&p, &coop_save::folder_of(&root, &p.id), &saveimport::locate(&dir).0, start_as.unwrap_or_default())?;
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
        for k in ["id", "name", "members", "me", "room_code", "relay"] {
            assert!(v["profile"].get(k).is_some(), "profile.{}", k);
        }
        let f = serde_json::to_value(crate::coop_diag::Finding { mismatch: crate::coop_diag::Mismatch::Mods, own_mods: String::new() }).unwrap();
        assert_eq!(f["mismatch"], "mods");
    }
}
