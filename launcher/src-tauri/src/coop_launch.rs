// SPDX-License-Identifier: GPL-3.0-or-later
//! How the launcher tells the game it is a co-op start, and which member plays.
//!
//! Everything goes through the game's environment, for this one start: the runtime lets `NXRT_<SECTION>_<KEY>` win over pkp.ini
//! (runtime/docs/CONFIG.md), and nothing is written to pkp.ini or profiles.json, so the next singleplayer start finds no trace. The
//! variables of a co-op start (the contract the game layer reads):
//!
//!   NXRT_PKP_COOP=1              co-op mode (`[pkp] coop`: set only by the launcher, per start, no in-game row)
//!   NXRT_PKP_COOP_ROLE=host|visit    what this member does this session (`[pkp] coop_role`): the newest copy hosts (coop_save::choose_host)
//!   NXRT_PKP_COOP_GROUP=<id>     the co-op save's id, also the name of its folder (`[pkp] coop_group`); equals `group` in coop.marker
//!   NXRT_PKP_COOP_MEMBER=<id>    this PC's member, as in coop.json (`[pkp] coop_member`); the same 32 hex digits are [net] uid
//!   NXRT_PKP_COOP_MEMBERS=<n>    how many members the group has (`[pkp] coop_members`); the session waits for all of them
//!   NXRT_FS_SAVE_DIR=<folder>    the co-op folder is the whole save folder of this run (coop_save.rs has its layout)
//!   NXRT_ACCOUNT_UID, NXRT_ACCOUNT_NICKNAME    the shared account of the save, and this member's name. The account also picks the
//!                                     group's own mod set: the Modtopia core reads the overrides of the save's account (mods_scope.rs)
//!   NXRT_NET_PROTOCOL=native, NXRT_NET_MODE=auto, NXRT_NET_INTERNET=true, NXRT_NET_RELAY=<relay>, NXRT_NET_UID=<member id>
//!   NXRT_NET_PRIVATE_ROOM=<room code> the private room on the relay (`[net] private_room`): not listed there, members only
//!                                     The co-op module takes a fixed code as is (coop_cfg_parse_ex `room`, same alphabet as the code
//!                                     made here) and then does not list the room (`x_list` is only for a new code)
//!
//! A start that is not a co-op start removes the five NXRT_PKP_COOP* variables from the game's environment (`apply` with None), so a
//! launcher that was itself started with them set cannot turn a singleplayer start into a co-op one.

use std::path::Path;
use std::process::Command;

use crate::coop::Profile;
use crate::coop_save::{self, HostChoice};

/// The switch variables of co-op mode: a singleplayer start never has them. The settings a co-op start also sets (NXRT_NET_*, NXRT_ACCOUNT_*,
/// NXRT_FS_SAVE_DIR) are ordinary runtime overrides a player may use on their own, so they are left alone.
const COOP_ONLY: [&str; 5] = ["NXRT_PKP_COOP", "NXRT_PKP_COOP_ROLE", "NXRT_PKP_COOP_GROUP", "NXRT_PKP_COOP_MEMBER", "NXRT_PKP_COOP_MEMBERS"];

/// The environment of one co-op start.
#[derive(Debug, PartialEq)]
pub struct Launch {
    env: Vec<(&'static str, String)>,
}

impl Launch {
    #[cfg(test)]
    pub fn get(&self, key: &str) -> Option<&str> {
        self.env.iter().find(|(k, _)| *k == key).map(|(_, v)| v.as_str())
    }
}

/// The co-op start of profile `p` from folder `dir`, after the refusals of coop_save::ensure_playable. `singleplayer_save_dir`: the
/// save folder a singleplayer start uses (never the same folder as `dir`). The role comes from the copies' stamps.
pub fn build(p: &Profile, dir: &Path, singleplayer_save_dir: &Path) -> Result<(Launch, HostChoice), String> {
    coop_save::ensure_playable(dir, &p.id, singleplayer_save_dir)?;
    let me = p.member(&p.me).ok_or("This co-op save has no player for this PC.")?;
    let choice = coop_save::choose_host(&p.member_ids(), &p.me, coop_save::own_stamp(dir), &coop_save::peer_stamps(dir));
    let env = vec![
        ("NXRT_PKP_COOP", "1".to_string()),
        ("NXRT_PKP_COOP_ROLE", choice.role.word().to_string()),
        ("NXRT_PKP_COOP_GROUP", p.id.clone()),
        ("NXRT_PKP_COOP_MEMBER", me.id.clone()),
        ("NXRT_PKP_COOP_MEMBERS", p.members.len().to_string()),
        ("NXRT_FS_SAVE_DIR", dir.display().to_string()),
        ("NXRT_ACCOUNT_UID", p.account_uid.clone()),
        ("NXRT_ACCOUNT_NICKNAME", crate::profiles::nickname_of(&me.name)),
        ("NXRT_NET_PROTOCOL", "native".to_string()),
        ("NXRT_NET_MODE", "auto".to_string()),
        ("NXRT_NET_INTERNET", "true".to_string()),
        ("NXRT_NET_RELAY", p.relay.clone()),
        ("NXRT_NET_UID", me.id.clone()),
        ("NXRT_NET_PRIVATE_ROOM", p.room_code.clone()),
    ];
    Ok((Launch { env }, choice))
}

/// Puts `launch` into the game's environment; None (a singleplayer start) takes every co-op-only variable out of it.
pub fn apply(cmd: &mut Command, launch: Option<&Launch>) {
    match launch {
        Some(l) => {
            for (k, v) in &l.env {
                cmd.env(k, v);
            }
        }
        None => {
            for k in COOP_ONLY {
                cmd.env_remove(k);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsStr;

    fn setup() -> (Profile, std::path::PathBuf, std::path::PathBuf) {
        static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!("pkp-cooplaunch-{}-{}", std::process::id(), N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
        let _ = std::fs::remove_dir_all(&root);
        let sp = root.join("save");
        std::fs::create_dir_all(&sp).unwrap();
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
        let (l, choice) = build(&p, &dir, &sp).unwrap();
        assert_eq!(l.get("NXRT_PKP_COOP"), Some("1"));
        assert_eq!(l.get("NXRT_PKP_COOP_GROUP"), Some(p.id.as_str()));
        assert_eq!(l.get("NXRT_PKP_COOP_MEMBER"), Some(p.me.as_str()));
        assert_eq!(l.get("NXRT_PKP_COOP_MEMBERS"), Some("2"));
        assert_eq!(l.get("NXRT_NET_UID"), Some(p.me.as_str()));
        assert_eq!(l.get("NXRT_FS_SAVE_DIR"), Some(dir.display().to_string().as_str()));
        assert_eq!(l.get("NXRT_NET_PRIVATE_ROOM"), Some(p.room_code.as_str()));
        assert_eq!(l.get("NXRT_NET_RELAY"), Some("192.0.2.10"));
        // nothing known yet: the creator hosts
        assert_eq!(l.get("NXRT_PKP_COOP_ROLE"), Some("host"));
        assert_eq!(choice.host, p.members[0].id);
    }

    #[test]
    fn a_member_with_an_older_copy_visits() {
        let (mut p, dir, sp) = setup();
        std::fs::write(dir.join(coop_save::PEERS), format!(r#"{{"peers":{{"{}":{{"seq":3,"saved_at":9}}}}}}"#, p.members[1].id)).unwrap();
        let (l, c) = build(&p, &dir, &sp).unwrap();
        assert_eq!(l.get("NXRT_PKP_COOP_ROLE"), Some("visit"));
        assert_eq!(c.host, p.members[1].id);
        // the same folder, played by the other member whose copy is that one
        p.me = p.members[1].id.clone();
        std::fs::write(dir.join(coop_save::STAMP), r#"{"seq":3,"saved_at":9}"#).unwrap();
        std::fs::write(dir.join(coop_save::PEERS), format!(r#"{{"peers":{{"{}":{{"seq":1,"saved_at":1}}}}}}"#, p.members[0].id)).unwrap();
        assert_eq!(build(&p, &dir, &sp).unwrap().0.get("NXRT_PKP_COOP_ROLE"), Some("host"));
    }

    #[test]
    fn a_folder_that_is_not_the_co_op_save_is_refused() {
        let (p, dir, sp) = setup();
        assert!(build(&p, &sp, &sp).is_err(), "the singleplayer folder is never a co-op save");
        std::fs::remove_file(dir.join(coop_save::MARKER)).unwrap();
        assert!(build(&p, &dir, &sp).is_err());
    }

    #[test]
    fn apply_sets_a_co_op_start_and_clears_a_singleplayer_one() {
        let (p, dir, sp) = setup();
        let (l, _) = build(&p, &dir, &sp).unwrap();
        let mut cmd = Command::new("pkp-test");
        apply(&mut cmd, Some(&l));
        let e = env_of(&cmd);
        assert!(e.contains(&("NXRT_PKP_COOP".into(), Some("1".into()))));
        let mut single = Command::new("pkp-test");
        apply(&mut single, None);
        for k in COOP_ONLY {
            let v = single.get_envs().find(|(n, _)| *n == OsStr::new(k));
            assert_eq!(v, Some((OsStr::new(k), None)), "{} is removed", k);
        }
        assert!(single.get_envs().all(|(n, _)| n != OsStr::new("NXRT_FS_SAVE_DIR")), "the player's own save folder is untouched");
    }
}
