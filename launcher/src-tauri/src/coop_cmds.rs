// SPDX-License-Identifier: GPL-3.0-or-later
//! Co-op commands (coop.rs holds the profiles, coop_save.rs the folder and who hosts, coop_launch.rs what the game is told).

use serde::Serialize;
use std::path::{Path, PathBuf};
use tauri::AppHandle;

use crate::config::game_dir;
use crate::coop::{self, Profile};
use crate::coop_save::{self, HostChoice};
use crate::guard::{ensure_idle, Need};
use crate::states::States;
use crate::{coop_launch, launch, play, saveimport};

/// A co-op save as the page shows it: the profile, who would host if it started now, and where it lives.
#[derive(Serialize)]
pub struct CoopView {
    pub profile: Profile,
    pub host: HostChoice,
    pub folder: String,
}

fn view(root: &Path, p: Profile) -> CoopView {
    let dir = coop_save::folder_of(root, &p.id);
    let host = coop_save::choose_host(&p.member_ids(), &p.me, coop_save::own_stamp(&dir), &coop_save::peer_stamps(&dir));
    CoopView { folder: crate::dump::fwd(&dir), profile: p, host }
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

/// The players an invite names (creator first), so the page can ask which one you are. Empty: not an invite.
#[tauri::command]
pub fn coop_invite_members(invite: String) -> Vec<coop::Member> {
    coop::invite_members(&invite).unwrap_or_default()
}

/// Joins from an invite as `member`, a player the creator added.
#[tauri::command]
pub fn coop_accept(app: AppHandle, invite: String, member: String) -> Result<Vec<CoopView>, String> {
    let root = guarded_root(&app)?;
    coop::accept(&root, &invite, &member)?;
    Ok(views(&root))
}

/// Starts the game as a member of co-op save `id`: its own folder as the save folder, the private room, and who hosts.
#[tauri::command]
pub fn coop_play(app: AppHandle, id: String) -> Result<(), String> {
    let updates = app.updates();
    let _swap = updates.hold_swap();
    let (dir, cfg) = launch::prepare(&app)?;
    let root = coop::root_of(&dir);
    let p = coop::get(&root, &id)?;
    // the group's own mod set is the layer of its account (mods_scope.rs); the game picks it from NXRT_ACCOUNT_UID
    crate::mods::ensure_points_built(&dir, &crate::mods::Scope::Coop(p.id.clone()))?;
    let (go, _) = coop_launch::build(&p, &coop_save::folder_of(&root, &p.id), &saveimport::locate(&dir).0)?;
    play::start(&app, &dir, &cfg, false, Some(go))
}
