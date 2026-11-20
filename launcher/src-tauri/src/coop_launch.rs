// SPDX-License-Identifier: GPL-3.0-or-later
//! How the launcher tells the game it is a co-op start, and which member plays.
//!
//! Everything goes through the game's environment, for this one start: the runtime lets `NXRT_<SECTION>_<KEY>` win over pkp.ini
//! (runtime/docs/CONFIG.md), and nothing is written to pkp.ini or profiles.json, so the next singleplayer start finds no trace. The
//! variables of a co-op start (the contract the game layer reads):
//!
//!   NXRT_PKP_COOP=1              co-op mode (`[pkp] coop`: set only by the launcher, per start, no in-game row)
//!   NXRT_PKP_COOP_ROLE=host|visit    what this member does this session (`[pkp] coop_role`): the newest copy hosts (coop_save::choose_host)
//!                                     unless an online player picked Host or Join for this start (coop_save::StartAs); in a split
//!                                     screen save window 1 always hosts (coop_save::split_host)
//!   NXRT_PKP_COOP_GROUP=<id>     the co-op save's id, also the name of its folder (`[pkp] coop_group`); equals `group` in coop.marker
//!   NXRT_PKP_COOP_MEMBER=<id>    this window's member, as in coop.json (`[pkp] coop_member`); the same 32 hex digits are [net] uid
//!   NXRT_PKP_COOP_MEMBERS=<n>    how many members the group has (`[pkp] coop_members`); the session waits for all of them
//!   NXRT_FS_SAVE_DIR=<folder>    the co-op folder is the whole save folder of this run (coop_save.rs has its layout)
//!   NXRT_ACCOUNT_UID, NXRT_ACCOUNT_NICKNAME    the shared account of the save, and this member's name. The account also picks the
//!                                     group's own mod set: the Modtopia core reads the overrides of the save's account (mods_scope.rs)
//!   NXRT_NET_PROTOCOL=native, NXRT_NET_UID=<member id>; online: NXRT_NET_MODE=auto, NXRT_NET_INTERNET=true, NXRT_NET_RELAY=<relay>
//!   NXRT_NET_PRIVATE_ROOM=<room code> the private room on the relay (`[net] private_room`): not listed there, members only
//!                                     The co-op module takes a fixed code as is (coop_cfg_parse_ex `room`, same alphabet as the code
//!                                     made here) and then does not list the room (`x_list` is only for a new code)
//!   NXRT_PKP_LINK_INTERACT_EVERYWHERE=0   Link Play's "Interact everywhere" is part of the mods fingerprint players are matched on, so a
//!                                     member with it on never finds the host. Co-op sets its own permissions, so it is off for every co-op start
//!
//! Split screen gives each member its own process and save folder. Visitors join the host at 127.0.0.1, with internet and relay off.
//! Each process gets its window, controller and audio settings through NXRT_SPLIT_* (runtime/include/nxrt/split.h).
//! Singleplayer clears the co-op and split switches inherited from the launcher's environment.

use serde::Deserialize;
use std::fs;
use std::io::ErrorKind;
use std::path::Path;
use std::process::Command;

use crate::coop::{self, Member, Profile};
use crate::coop_save::{self, HostChoice, Role, StartAs};
use crate::settings::CONTROLS_INI;

/// The switch variables of co-op mode: a singleplayer start never has them. The settings a co-op start also sets (NXRT_NET_*, NXRT_ACCOUNT_*,
/// NXRT_FS_SAVE_DIR) are ordinary runtime overrides a player may use on their own, so they are left alone.
const COOP_ONLY: [&str; 5] = ["NXRT_PKP_COOP", "NXRT_PKP_COOP_ROLE", "NXRT_PKP_COOP_GROUP", "NXRT_PKP_COOP_MEMBER", "NXRT_PKP_COOP_MEMBERS"];
const SPLIT_ONLY: [&str; 5] = ["NXRT_SPLIT_WINDOW", "NXRT_SPLIT_COUNT", "NXRT_SPLIT_LAYOUT", "NXRT_SPLIT_PAD", "NXRT_SPLIT_MUTE"];
const RELAY_ONLY: &[&str] = &["NXRT_NET_RELAY", "NXRT_NET_PRIVATE_ROOM"];
/// Separate from a normal Link Play host (47600) and a relay (47610).
const SPLIT_PORT: u16 = 47620;
pub const FIRST_LOG: &str = "pkp.log";

/// One player's process: its log relative to the game folder and its environment overrides.
#[derive(Debug, PartialEq)]
pub struct Launch {
    pub log: String,
    env: Vec<(&'static str, String)>,
    remove: &'static [&'static str],
}

impl Launch {
    #[cfg(test)]
    pub fn get(&self, key: &str) -> Option<&str> {
        self.env.iter().find(|(k, _)| *k == key).map(|(_, v)| v.as_str())
    }
}

pub struct Group {
    pub first: Launch,
    pub others: Vec<Launch>,
}

#[derive(Deserialize, Default, Clone, Debug)]
#[serde(default)]
pub struct SplitOptions {
    /// Side by side for two windows; the first takes the left half for three.
    pub side: bool,
    /// What each window plays with, in window order: 0 the keyboard and mouse only, k the k-th controller connected
    /// (`[split] pad`). A window without a usable choice gets the first controller no window plays (window_pads).
    pub pads: Vec<u8>,
    /// The other windows play sound too.
    pub sound: bool,
}

/// The co-op start of profile `p` from folder `dir`, after the refusals of coop_save::ensure_playable. `singleplayer_save_dir`: the
/// save folder a singleplayer start uses (never the same folder as `dir`). The role comes from the copies' stamps, or from `start_as` when
/// the player picked one.
pub fn build(p: &Profile, dir: &Path, singleplayer_save_dir: &Path, start_as: StartAs) -> Result<(Group, HostChoice), String> {
    coop_save::ensure_playable(dir, &p.id, singleplayer_save_dir)?;
    let me = p.member(&p.me).ok_or("This co-op save has no player for this PC.")?;
    let choice = coop_save::choose_host(&p.member_ids(), &p.me, coop_save::own_stamp(dir), &coop_save::peer_stamps(dir));
    let mut env = member_env(p, me, dir, coop_save::role_for(&choice, start_as), crate::profiles::nickname_of(&me.name));
    env.extend([
        ("NXRT_NET_MODE", "auto".into()),
        ("NXRT_NET_INTERNET", "true".into()),
        ("NXRT_NET_RELAY", p.relay.clone()),
        ("NXRT_NET_PRIVATE_ROOM", p.room_code.clone()),
    ]);
    Ok((Group { first: Launch { log: FIRST_LOG.into(), env, remove: &[] }, others: Vec::new() }, choice))
}

fn member_env(p: &Profile, member: &Member, dir: &Path, role: Role, nickname: String) -> Vec<(&'static str, String)> {
    vec![
        ("NXRT_PKP_COOP", "1".to_string()),
        ("NXRT_PKP_COOP_ROLE", role.word().to_string()),
        ("NXRT_PKP_COOP_GROUP", p.id.clone()),
        ("NXRT_PKP_COOP_MEMBER", member.id.clone()),
        ("NXRT_PKP_COOP_MEMBERS", p.members.len().to_string()),
        ("NXRT_FS_SAVE_DIR", dir.display().to_string()),
        ("NXRT_ACCOUNT_UID", p.account_uid.clone()),
        ("NXRT_ACCOUNT_NICKNAME", nickname),
        ("NXRT_NET_PROTOCOL", "native".to_string()),
        ("NXRT_NET_UID", member.id.clone()),
        ("NXRT_PKP_LINK_INTERACT_EVERYWHERE", "0".to_string()),
    ]
}

pub fn build_split(p: &Profile, root: &Path, singleplayer_save_dir: &Path, opt: &SplitOptions) -> Result<(Group, HostChoice), String> {
    if !p.split {
        return Err("This is not a split screen save.".into());
    }
    let count = p.members.len();
    if !(2..=coop::MAX_MEMBERS).contains(&count) {
        return Err(coop::SPLIT_PLAYER_COUNT.into());
    }
    let dirs: Vec<_> = coop_save::split_folders(root, p).collect();
    for dir in &dirs {
        coop_save::ensure_playable(dir, &p.id, singleplayer_save_dir)?;
    }
    let choice = coop_save::host_of(root, p);
    let layout = if opt.side && count < 4 { "side" } else { "stacked" };
    let pads = window_pads(count, &opt.pads);
    let mut windows = p.members.iter().zip(&dirs).enumerate().map(|(i, (member, dir))| {
        let window = i + 1;
        let (role, mode) = if member.id == choice.host {
            (Role::Host, format!("auto:{}", SPLIT_PORT))
        } else {
            (Role::Visit, format!("join:127.0.0.1:{}", SPLIT_PORT))
        };
        let mut nickname = crate::profiles::nickname_of(&member.name);
        // The Windows runtime reads the ANSI environment; ASCII survives every code page.
        if nickname.is_empty() || (cfg!(windows) && !nickname.is_ascii()) {
            nickname = format!("Player {}", window);
        }
        let mut env = member_env(p, member, dir, role, nickname);
        env.extend([
            ("NXRT_NET_MODE", mode),
            ("NXRT_NET_INTERNET", "false".into()),
            ("NXRT_SPLIT_WINDOW", window.to_string()),
            ("NXRT_SPLIT_COUNT", count.to_string()),
            ("NXRT_SPLIT_LAYOUT", layout.into()),
            ("NXRT_SPLIT_PAD", pads[i].to_string()),
            ("NXRT_SPLIT_MUTE", (window > 1 && !opt.sound).to_string()),
        ]);
        let log = if window == 1 {
            FIRST_LOG.into()
        } else {
            let folder = window_folder(window);
            env.push(("NXRT_PLAY_CONTROLS", format!("{}/{}", folder, CONTROLS_INI)));
            // Only window 1 writes the shared pipeline cache.
            env.push(("NXRT_NVN_PIPELINE_CACHE_SAVE", "off".into()));
            format!("{}/{}", folder, FIRST_LOG)
        };
        Launch { log, env, remove: RELAY_ONLY }
    });
    let first = windows.next().unwrap(); // the player count was checked above
    let others = windows.collect();
    Ok((Group { first, others }, choice))
}

/// What `count` windows play with: a window keeps its choice unless it is out of range or an earlier window has it (one keyboard,
/// one window per controller); a window without one gets the first controller nobody plays. The page makes the same choice
/// (src/coop-model.js windowPads).
fn window_pads(count: usize, wanted: &[u8]) -> Vec<u8> {
    let max = coop::MAX_MEMBERS as u8;
    let mut out: Vec<Option<u8>> = Vec::with_capacity(count);
    for i in 0..count {
        let w = wanted.get(i).copied().filter(|w| *w <= max && !out.contains(&Some(*w)));
        out.push(w);
    }
    for i in 0..count {
        if out[i].is_none() {
            out[i] = (1..=max).find(|k| !out.contains(&Some(*k)));
        }
    }
    out.into_iter().map(|p| p.unwrap_or(0)).collect()
}

fn window_folder(window: usize) -> String {
    format!("split/{}", window)
}

/// Each extra window starts with the current controls.ini, or defaults when there is none.
pub fn prepare_windows(game_dir: &Path, n: usize) -> Result<(), String> {
    let source = game_dir.join(CONTROLS_INI);
    for window in 2..=n {
        let dir = game_dir.join(window_folder(window));
        let error = |e| format!("Window {}'s settings could not be prepared: {}", window, e);
        fs::create_dir_all(&dir).map_err(error)?;
        let dest = dir.join(CONTROLS_INI);
        match fs::copy(&source, &dest) {
            Ok(_) => {}
            Err(e) if e.kind() == ErrorKind::NotFound => match fs::remove_file(&dest) {
                Ok(_) => {}
                Err(e) if e.kind() == ErrorKind::NotFound => {}
                Err(e) => return Err(error(e)),
            },
            Err(e) => return Err(error(e)),
        }
    }
    Ok(())
}

/// Applies a window's environment, or removes the co-op and split switches for a singleplayer start.
pub fn apply(cmd: &mut Command, launch: Option<&Launch>) {
    match launch {
        Some(l) => {
            for (k, v) in &l.env {
                cmd.env(k, v);
            }
            for k in l.remove {
                cmd.env_remove(k);
            }
        }
        None => {
            for k in COOP_ONLY.into_iter().chain(SPLIT_ONLY) {
                cmd.env_remove(k);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsStr;

    fn temp_save_dir() -> std::path::PathBuf {
        static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!("pkp-cooplaunch-{}-{}", std::process::id(), N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
        let _ = std::fs::remove_dir_all(&root);
        let sp = root.join("save");
        std::fs::create_dir_all(&sp).unwrap();
        sp
    }

    fn setup() -> (Profile, std::path::PathBuf, std::path::PathBuf) {
        let sp = temp_save_dir();
        let croot = crate::coop::root_beside(&sp);
        let p = crate::coop::create(&croot, "Island", "Ash", "192.0.2.10").unwrap();
        let p = crate::coop::add_member(&croot, &p.id, "Misty").unwrap();
        let dir = coop_save::folder_of(&croot, &p.id);
        (p, dir, sp)
    }

    fn env_of(cmd: &Command) -> Vec<(String, Option<String>)> {
        cmd.get_envs().map(|(k, v)| (k.to_string_lossy().into_owned(), v.map(|v| v.to_string_lossy().into_owned()))).collect()
    }

    #[test]
    fn a_co_op_start_names_the_mode_the_member_the_folder_and_the_room() {
        let (p, dir, sp) = setup();
        let (group, choice) = build(&p, &dir, &sp, StartAs::Auto).unwrap();
        assert!(group.others.is_empty());
        let l = group.first;
        assert_eq!(l.log, "pkp.log");
        assert_eq!(l.get("NXRT_PKP_COOP"), Some("1"));
        assert_eq!(l.get("NXRT_PKP_COOP_GROUP"), Some(p.id.as_str()));
        assert_eq!(l.get("NXRT_PKP_COOP_MEMBER"), Some(p.me.as_str()));
        assert_eq!(l.get("NXRT_PKP_COOP_MEMBERS"), Some("2"));
        assert_eq!(l.get("NXRT_NET_UID"), Some(p.me.as_str()));
        assert_eq!(l.get("NXRT_FS_SAVE_DIR"), Some(dir.display().to_string().as_str()));
        assert_eq!(l.get("NXRT_NET_PRIVATE_ROOM"), Some(p.room_code.as_str()));
        assert_eq!(l.get("NXRT_NET_RELAY"), Some("192.0.2.10"));
        assert_eq!(l.get("NXRT_NET_PROTOCOL"), Some("native"));
        assert_eq!(l.get("NXRT_NET_MODE"), Some("auto"));
        assert_eq!(l.get("NXRT_NET_INTERNET"), Some("true"));
        assert!(SPLIT_ONLY.iter().all(|k| l.get(k).is_none()));
        // nothing known yet: the creator hosts
        assert_eq!(l.get("NXRT_PKP_COOP_ROLE"), Some("host"));
        assert_eq!(choice.host, p.members[0].id);
        // Link Play's Interact everywhere is in the mods fingerprint: off, so no member hides the host from another
        assert_eq!(l.get("NXRT_PKP_LINK_INTERACT_EVERYWHERE"), Some("0"));
    }

    #[test]
    fn the_player_can_pick_host_or_join_for_one_start() {
        let (p, dir, sp) = setup();
        assert_eq!(build(&p, &dir, &sp, StartAs::Visit).unwrap().0.first.get("NXRT_PKP_COOP_ROLE"), Some("visit"));
        assert_eq!(build(&p, &dir, &sp, StartAs::Host).unwrap().0.first.get("NXRT_PKP_COOP_ROLE"), Some("host"));
    }

    #[test]
    fn a_member_with_an_older_copy_visits() {
        let (mut p, dir, sp) = setup();
        std::fs::write(dir.join(coop_save::PEERS), format!(r#"{{"peers":{{"{}":{{"seq":3,"saved_at":9}}}}}}"#, p.members[1].id)).unwrap();
        let (l, c) = build(&p, &dir, &sp, StartAs::Auto).unwrap();
        assert_eq!(l.first.get("NXRT_PKP_COOP_ROLE"), Some("visit"));
        assert_eq!(c.host, p.members[1].id);
        // the same folder, played by the other member whose copy is that one
        p.me = p.members[1].id.clone();
        std::fs::write(dir.join(coop_save::STAMP), r#"{"seq":3,"saved_at":9}"#).unwrap();
        std::fs::write(dir.join(coop_save::PEERS), format!(r#"{{"peers":{{"{}":{{"seq":1,"saved_at":1}}}}}}"#, p.members[0].id)).unwrap();
        assert_eq!(build(&p, &dir, &sp, StartAs::Auto).unwrap().0.first.get("NXRT_PKP_COOP_ROLE"), Some("host"));
    }

    #[test]
    fn a_folder_that_is_not_the_co_op_save_is_refused() {
        let (p, dir, sp) = setup();
        assert!(build(&p, &sp, &sp, StartAs::Auto).is_err(), "the singleplayer folder is never a co-op save");
        std::fs::remove_file(dir.join(coop_save::MARKER)).unwrap();
        assert!(build(&p, &dir, &sp, StartAs::Auto).is_err());
    }

    #[test]
    fn apply_sets_a_co_op_start_and_clears_a_singleplayer_one() {
        let (p, dir, sp) = setup();
        let (l, _) = build(&p, &dir, &sp, StartAs::Auto).unwrap();
        let mut cmd = Command::new("pkp-test");
        apply(&mut cmd, Some(&l.first));
        let e = env_of(&cmd);
        assert!(e.contains(&("NXRT_PKP_COOP".into(), Some("1".into()))));
        let mut single = Command::new("pkp-test");
        apply(&mut single, None);
        for k in COOP_ONLY.into_iter().chain(SPLIT_ONLY) {
            let v = single.get_envs().find(|(n, _)| *n == OsStr::new(k));
            assert_eq!(v, Some((OsStr::new(k), None)), "{} is removed", k);
        }
        assert!(single.get_envs().all(|(n, _)| n != OsStr::new("NXRT_FS_SAVE_DIR")), "the player's own save folder is untouched");
    }

    fn split_setup(count: usize) -> (Profile, std::path::PathBuf, std::path::PathBuf) {
        let sp = temp_save_dir();
        let root = coop::root_beside(&sp);
        let names: Vec<_> = (1..=count).map(|n| format!("Local {}", n)).collect();
        let p = coop::create_split(&root, "Local world", &names).unwrap();
        (p, root, sp)
    }

    #[test]
    fn a_two_window_split_uses_local_transport_and_separate_saves_and_logs() {
        let (p, root, sp) = split_setup(2);
        coop_save::ensure_split_folders(&root, &p).unwrap();
        let (group, choice) = build_split(&p, &root, &sp, &SplitOptions::default()).unwrap();
        assert_eq!(choice.host, p.me);
        assert_eq!(group.others.len(), 1);
        for (i, launch) in std::iter::once(&group.first).chain(&group.others).enumerate() {
            let member = &p.members[i];
            let (role, mode, folder) = if i == 0 {
                ("host", "auto:47620", coop_save::folder_of(&root, &p.id))
            } else {
                ("visit", "join:127.0.0.1:47620", coop_save::split_folder_of(&root, &p.id, &member.id))
            };
            for (key, value) in [
                ("NXRT_PKP_COOP", "1"),
                ("NXRT_PKP_COOP_ROLE", role),
                ("NXRT_PKP_COOP_GROUP", &p.id),
                ("NXRT_PKP_COOP_MEMBER", &member.id),
                ("NXRT_PKP_COOP_MEMBERS", "2"),
                ("NXRT_PKP_LINK_INTERACT_EVERYWHERE", "0"),
                ("NXRT_FS_SAVE_DIR", &folder.display().to_string()),
                ("NXRT_ACCOUNT_UID", &p.account_uid),
                ("NXRT_ACCOUNT_NICKNAME", &member.name),
                ("NXRT_NET_UID", &member.id),
                ("NXRT_NET_PROTOCOL", "native"),
                ("NXRT_NET_MODE", mode),
                ("NXRT_NET_INTERNET", "false"),
                ("NXRT_SPLIT_WINDOW", &(i + 1).to_string()),
                ("NXRT_SPLIT_COUNT", "2"),
                ("NXRT_SPLIT_LAYOUT", "stacked"),
                ("NXRT_SPLIT_PAD", &(i + 1).to_string()),
                ("NXRT_SPLIT_MUTE", if i == 0 { "false" } else { "true" }),
            ] {
                assert_eq!(launch.get(key), Some(value), "window {}: {}", i + 1, key);
            }
            let mut cmd = Command::new("pkp-test");
            for key in RELAY_ONLY {
                cmd.env(key, "inherited");
                assert_eq!(launch.get(key), None);
            }
            apply(&mut cmd, Some(launch));
            for key in RELAY_ONLY {
                assert_eq!(cmd.get_envs().find(|(k, _)| *k == OsStr::new(key)), Some((OsStr::new(key), None)));
            }
        }
        assert_eq!(group.first.log, "pkp.log");
        assert_eq!(group.first.get("NXRT_PLAY_CONTROLS"), None);
        assert_eq!(group.first.get("NXRT_NVN_PIPELINE_CACHE_SAVE"), None);
        assert_eq!(group.others[0].log, "split/2/pkp.log");
        assert_eq!(group.others[0].get("NXRT_PLAY_CONTROLS"), Some("split/2/controls.ini"));
        assert_eq!(group.others[0].get("NXRT_NVN_PIPELINE_CACHE_SAVE"), Some("off"));
    }

    #[test]
    fn a_four_window_split_hosts_in_window_one_and_uses_the_chosen_pads_and_sound() {
        let (p, root, sp) = split_setup(4);
        coop_save::ensure_split_folders(&root, &p).unwrap();
        let newest = coop_save::split_folder_of(&root, &p.id, &p.members[2].id);
        fs::write(newest.join(coop_save::STAMP), r#"{"seq":7,"saved_at":10}"#).unwrap();
        let opts = serde_json::from_str(r#"{"side":true,"pads":[0,1,2,3],"sound":true}"#).unwrap();
        let (group, choice) = build_split(&p, &root, &sp, &opts).unwrap();
        assert_eq!(choice.host, p.members[0].id, "player 1 hosts even when another copy is newer");
        assert_eq!(choice.role, Role::Host);
        assert_eq!(group.others.len(), 3);
        for (i, launch) in std::iter::once(&group.first).chain(&group.others).enumerate() {
            let window = i + 1;
            let (role, mode) = if window == 1 { ("host", "auto:47620") } else { ("visit", "join:127.0.0.1:47620") };
            let dir = if window == 1 { coop_save::folder_of(&root, &p.id) } else { coop_save::split_folder_of(&root, &p.id, &p.members[i].id) };
            assert_eq!(launch.get("NXRT_PKP_COOP_ROLE"), Some(role));
            assert_eq!(launch.get("NXRT_PKP_COOP_MEMBER"), Some(p.members[i].id.as_str()));
            assert_eq!(launch.get("NXRT_FS_SAVE_DIR"), Some(dir.display().to_string().as_str()));
            assert_eq!(launch.get("NXRT_NET_MODE"), Some(mode));
            assert!(RELAY_ONLY.iter().all(|k| launch.get(k).is_none()));
            assert_eq!(launch.get("NXRT_SPLIT_WINDOW"), Some(window.to_string().as_str()));
            assert_eq!(launch.get("NXRT_SPLIT_COUNT"), Some("4"));
            assert_eq!(launch.get("NXRT_SPLIT_PAD"), Some(i.to_string().as_str()));
            assert_eq!(launch.get("NXRT_SPLIT_LAYOUT"), Some("stacked"));
            assert_eq!(launch.get("NXRT_SPLIT_MUTE"), Some("false"));
            if window > 1 {
                assert_eq!(launch.log, format!("split/{}/pkp.log", window));
            }
        }
    }

    #[test]
    fn each_window_plays_what_was_chosen_and_never_shares_a_controller() {
        assert_eq!(window_pads(3, &[]), vec![1, 2, 3], "controllers in window order");
        assert_eq!(window_pads(3, &[0]), vec![0, 1, 2], "keyboard for window 1: the controllers start at window 2");
        assert_eq!(window_pads(2, &[2, 0]), vec![2, 0], "window 2 on the keyboard");
        assert_eq!(window_pads(2, &[2, 2]), vec![2, 1], "a controller twice: the later window gets a free one");
        assert_eq!(window_pads(2, &[0, 0, 3]), vec![0, 1], "one keyboard; a choice for a missing window is dropped");
        assert_eq!(window_pads(2, &[9, 7]), vec![1, 2], "out of range is replaced");
    }

    #[test]
    fn side_layout_and_playable_markers_apply_to_each_window() {
        let (p, root, sp) = split_setup(3);
        let opts = serde_json::from_str::<SplitOptions>(r#"{"side":true}"#).unwrap();
        assert!(opts.pads.is_empty() && !opts.sound);
        assert!(build_split(&p, &root, &sp, &opts).is_err(), "the extra copies need markers");
        coop_save::ensure_split_folders(&root, &p).unwrap();
        let (group, _) = build_split(&p, &root, &sp, &opts).unwrap();
        for launch in std::iter::once(&group.first).chain(&group.others) {
            assert_eq!(launch.get("NXRT_SPLIT_LAYOUT"), Some("side"));
        }
        let second = coop_save::split_folder_of(&root, &p.id, &p.members[1].id);
        fs::remove_file(second.join(coop_save::MARKER)).unwrap();
        assert!(build_split(&p, &root, &sp, &opts).is_err());
    }

    #[test]
    fn windows_refresh_controls_from_the_game_folder() {
        let (_, _, dir) = split_setup(3);
        prepare_windows(&dir, 3).unwrap();
        let source = dir.join(CONTROLS_INI);
        let copies = [dir.join("split/2/controls.ini"), dir.join("split/3/controls.ini")];
        for text in ["width = 1280", "width = 1600"] {
            fs::write(&source, text).unwrap();
            prepare_windows(&dir, 3).unwrap();
            for copy in &copies {
                assert_eq!(fs::read_to_string(copy).unwrap(), text);
            }
        }
        fs::remove_file(source).unwrap();
        prepare_windows(&dir, 3).unwrap();
        assert!(copies.iter().all(|copy| !copy.exists()));
    }
}
