// SPDX-License-Identifier: GPL-3.0-or-later
//! A co-op save's folder: the marker that says "co-op save", the save stamp its files carry, the refusal both ways, and which member's
//! copy hosts the next session.
//!
//! Folder contract (the game layer and the game-side copy channel read and write the same files):
//!
//!   <coop folder>/                  the runtime's `[fs] save_dir` for a co-op start (NXRT_FS_SAVE_DIR), nothing else uses it
//!     coop.marker                   JSON `{ "kind": "pokopia-recomp-coop-save", "version": 1, "group": "<profile id>" }`. Written first when
//!                                   the folder is made. A folder with it is a co-op save, one without it never is (see `refuse_*`)
//!     coop.json                     the profile: members, room, relay (coop.rs)
//!     account/<key>/...             the world and story, as the game saves them: this PC's copy of the shared co-op save
//!     stamp.json                    this copy's stamp: JSON `{ "seq": u64, "saved_at": u64, "by": "<member id>" }`. `seq` counts the
//!                                   save points of the group (the authority adds 1 at each), `saved_at` is unix seconds. Whoever writes a copy
//!                                   (a save point, or a received copy) writes `stamp.json` LAST, so a stamp always names a whole copy
//!     peers.json                    what this PC last learned about the other members' copies: JSON `{ "peers": { "<member id>": { "seq", "saved_at" } } }`.
//!                                   Written by the game side at the end of a session; absent before the first one
//!     incoming/                     a copy being received; the game side moves it over `account/` and then writes `stamp.json`. The launcher
//!                                   never reads it, so a copy cut short is never played
//!     split/<member id>/            in a split screen save, each member after the first has a separate save folder here, with the same
//!                                   group marker and its own account/, stamp.json, peers.json and incoming/. The first uses <coop folder>/.
//!
//! The newest copy hosts: the greatest `(seq, saved_at)`, ties to the member who comes first in the member list (the creator first), so
//! every launcher that knows the same stamps picks the same host. A copy without `stamp.json` is stamped from its newest file time
//! with `seq` 0; no save at all is `(0, 0)`. A split screen save is the exception: player 1 (window 1) always hosts (split_host).

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use crate::coop::Profile;

pub const MARKER: &str = "coop.marker";
pub const STAMP: &str = "stamp.json";
pub const PEERS: &str = "peers.json";
const KIND: &str = "pokopia-recomp-coop-save";
const VERSION: u32 = 1;
/// The account folders of the game inside a save folder (the runtime's layout, saveimport.rs).
const ACCOUNT: &str = "account";

#[derive(Serialize, Deserialize)]
struct Marker {
    kind: String,
    version: u32,
    group: String,
}

/// Save stamp: ordered by `seq`, then `saved_at`. `by` names the member that wrote it (informational, never compared).
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct Stamp {
    pub seq: u64,
    pub saved_at: u64,
}

#[derive(Serialize, Deserialize, Default)]
struct StampFile {
    seq: u64,
    saved_at: u64,
    #[serde(default)]
    by: String,
}

#[derive(Serialize, Deserialize, Default)]
struct PeersFile {
    peers: HashMap<String, Stamp>,
}

// ---- marker and refusal ------------------------------------------------------------------------------------------------------------

/// The group (profile id) named by the folder's marker; None when the folder has no readable marker.
pub fn marker_group(dir: &Path) -> Option<String> {
    let m: Marker = serde_json::from_str(&fs::read_to_string(dir.join(MARKER)).ok()?).ok()?;
    (m.kind == KIND && m.version == VERSION && !m.group.is_empty()).then_some(m.group)
}

/// True when `dir` carries the co-op marker (readable or not: a damaged marker still means "never singleplayer").
pub fn is_coop_folder(dir: &Path) -> bool {
    dir.join(MARKER).exists()
}

/// Writes the marker of a new co-op folder.
pub fn write_marker(dir: &Path, group: &str) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&Marker { kind: KIND.into(), version: VERSION, group: group.into() }).map_err(|e| e.to_string())?;
    crate::settings::write_atomic(&dir.join(MARKER), &text)
}

/// Refusal one: a singleplayer start (Play, `--play`, a save import) never uses a co-op folder as its save folder.
pub fn refuse_as_singleplayer(save_dir: &Path) -> Result<(), String> {
    if is_coop_folder(save_dir) {
        return Err(format!(
            "The save folder {} holds a co-op save, and a co-op save is only played from Recompia: Co-op. Choose another save folder in the settings.",
            crate::dump::fwd(save_dir)
        ));
    }
    Ok(())
}

/// Refusal two: a folder becomes a co-op save only when the launcher makes it empty and marks it. A folder that already holds anything
/// is never adopted, so a singleplayer save cannot be turned into a co-op one. Creates the folder.
pub fn make_coop_folder(dir: &Path, group: &str) -> Result<(), String> {
    if dir.exists() && fs::read_dir(dir).map_err(|e| e.to_string())?.next().is_some() {
        return Err(format!("{} already has files in it. A co-op save starts in a new, empty folder.", crate::dump::fwd(dir)));
    }
    fs::create_dir_all(dir).map_err(|e| format!("Cannot make {}: {}", crate::dump::fwd(dir), e))?;
    write_marker(dir, group)
}

/// Before a co-op start: the folder has this group's marker, and it is not the singleplayer save folder, inside it, or around it.
pub fn ensure_playable(dir: &Path, group: &str, singleplayer_save_dir: &Path) -> Result<(), String> {
    match marker_group(dir) {
        Some(g) if g == group => {}
        Some(_) => return Err("This co-op folder belongs to another co-op save.".into()),
        None => return Err(format!("{} is not marked as a co-op save, so it is not played as one.", crate::dump::fwd(dir))),
    }
    let (a, b) = (fs::canonicalize(dir).unwrap_or_else(|_| dir.to_path_buf()), fs::canonicalize(singleplayer_save_dir).unwrap_or_else(|_| singleplayer_save_dir.to_path_buf()));
    if a.starts_with(&b) || b.starts_with(&a) {
        return Err("The co-op save folder and the singleplayer save folder overlap. Choose another save folder in the settings.".into());
    }
    Ok(())
}

// ---- the stamp ---------------------------------------------------------------------------------------------------------------------

fn newest_file_time(dir: &Path) -> u64 {
    let mut best = 0;
    if let Ok(rd) = fs::read_dir(dir) {
        for e in rd.flatten() {
            let Ok(md) = e.metadata() else { continue };
            let t = if md.is_dir() { newest_file_time(&e.path()) } else { md.modified().ok().and_then(|m| m.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs()) };
            best = best.max(t);
        }
    }
    best
}

/// This PC's copy: `stamp.json` when it reads, else the newest file time under `account/` with `seq` 0, else `(0, 0)`.
pub fn own_stamp(dir: &Path) -> Stamp {
    if let Some(s) = fs::read_to_string(dir.join(STAMP)).ok().and_then(|t| serde_json::from_str::<StampFile>(&t).ok()) {
        return Stamp { seq: s.seq, saved_at: s.saved_at };
    }
    Stamp { seq: 0, saved_at: newest_file_time(&dir.join(ACCOUNT)) }
}

/// What the last session learned about the other members' copies (empty when it never wrote `peers.json` or it does not read).
pub fn peer_stamps(dir: &Path) -> HashMap<String, Stamp> {
    fs::read_to_string(dir.join(PEERS)).ok().and_then(|t| serde_json::from_str::<PeersFile>(&t).ok()).map(|p| p.peers).unwrap_or_default()
}

// ---- who hosts ---------------------------------------------------------------------------------------------------------------------

#[derive(Serialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    Host,
    Visit,
}

impl Role {
    pub fn word(self) -> &'static str {
        match self {
            Role::Host => "host",
            Role::Visit => "visit",
        }
    }
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct HostChoice {
    /// The member whose copy is the newest.
    pub host: String,
    /// What this PC's member does (window 1 for split screen).
    pub role: Role,
    pub own: Stamp,
    /// True when a stamp of another member was known, from peers.json online or from the other copy on this PC in split screen.
    pub peers_known: bool,
}

/// The newest copy hosts. `members` are ids in list order; `me` is this PC's member. Every PC must reach the same answer, so the
/// stamps are compared only when every other member's stamp is known (peers.json). Otherwise the first member (the group's creator)
/// hosts: comparing this PC's real stamp with an unknown one made every member with a copy pick itself, and nobody visited.
pub fn choose_host(members: &[String], me: &str, own: Stamp, peers: &HashMap<String, Stamp>) -> HostChoice {
    let stamp_of = |id: &str| if id == me { own } else { peers.get(id).copied().unwrap_or_default() };
    let all_known = members.iter().all(|id| id == me || peers.contains_key(id));
    let host = if all_known {
        // max_by_key keeps the LAST of equal keys, so walk the list backwards: the earliest member wins a tie
        members.iter().rev().max_by_key(|id| stamp_of(id)).cloned()
    } else {
        members.first().cloned()
    }
    .unwrap_or_else(|| me.to_string());
    let peers_known = members.iter().any(|id| id != me && peers.contains_key(id));
    HostChoice { role: if host == me { Role::Host } else { Role::Visit }, host, own, peers_known }
}

/// A split screen save's host: always player 1 (window 1), whose copy, the save folder itself, is the group's world. The other
/// players' copies are visitors' copies, so none of them hosts whatever its files' times say. `own`: player 1's stamp.
pub fn split_host(members: &[String], own: Stamp) -> HostChoice {
    HostChoice { host: members.first().cloned().unwrap_or_default(), role: Role::Host, own, peers_known: false }
}

/// The next host of a save as it stands on disk.
pub fn host_of(root: &Path, p: &Profile) -> HostChoice {
    let members = p.member_ids();
    if p.split {
        split_host(&members, own_stamp(&folder_of(root, &p.id)))
    } else {
        let dir = folder_of(root, &p.id);
        choose_host(&members, &p.me, own_stamp(&dir), &peer_stamps(&dir))
    }
}

/// How the player starts this session: as the launcher decides (`Auto`, choose_host), or as host or visitor because they said so. The choice
/// is made per start on the page and never stored: a stored override went stale and let two members host the same session.
#[derive(Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum StartAs {
    #[default]
    Auto,
    Host,
    Visit,
}

/// The role this start plays: the automatic choice unless the player overrode it.
pub fn role_for(choice: &HostChoice, start_as: StartAs) -> Role {
    match start_as {
        StartAs::Auto => choice.role,
        StartAs::Host => Role::Host,
        StartAs::Visit => Role::Visit,
    }
}

/// The folder of group `id` under `root`.
pub fn folder_of(root: &Path, id: &str) -> PathBuf {
    root.join(id)
}

/// The private copy of a split screen member after the first.
pub fn split_folder_of(root: &Path, id: &str, member: &str) -> PathBuf {
    folder_of(root, id).join("split").join(member)
}

pub fn split_folders<'a>(root: &'a Path, p: &'a Profile) -> impl Iterator<Item = PathBuf> + 'a {
    std::iter::once(folder_of(root, &p.id)).chain(p.members.iter().skip(1).map(|m| split_folder_of(root, &p.id, &m.id)))
}

/// Marks each new split copy; an existing marked copy keeps its files.
pub fn ensure_split_folders(root: &Path, p: &Profile) -> Result<(), String> {
    for dir in split_folders(root, p).skip(1) {
        if marker_group(&dir).as_deref() != Some(p.id.as_str()) {
            make_coop_folder(&dir, &p.id)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!("pkp-coopsave-{}-{}-{}", tag, std::process::id(), N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn ids(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn a_new_folder_is_marked_and_a_used_one_is_never_adopted() {
        let root = tmp("mark");
        let d = root.join("g1");
        make_coop_folder(&d, "g1").unwrap();
        assert_eq!(marker_group(&d).as_deref(), Some("g1"));
        assert!(is_coop_folder(&d));
        // a singleplayer save folder (anything in it) cannot become a co-op save
        let sp = root.join("save");
        fs::create_dir_all(sp.join("account").join("x")).unwrap();
        assert!(make_coop_folder(&sp, "g2").is_err());
        assert!(!is_coop_folder(&sp));
    }

    #[test]
    fn a_co_op_folder_is_refused_as_singleplayer() {
        let d = tmp("refuse");
        assert!(refuse_as_singleplayer(&d).is_ok());
        write_marker(&d, "g").unwrap();
        assert!(refuse_as_singleplayer(&d).is_err());
        // a damaged marker still refuses
        fs::write(d.join(MARKER), "junk").unwrap();
        assert!(refuse_as_singleplayer(&d).is_err());
    }

    #[test]
    fn a_co_op_start_needs_its_own_marker_and_its_own_place() {
        let root = tmp("play");
        let sp = root.join("save");
        fs::create_dir_all(&sp).unwrap();
        let g = root.join("coop-saves").join("g1");
        make_coop_folder(&g, "g1").unwrap();
        assert!(ensure_playable(&g, "g1", &sp).is_ok());
        assert!(ensure_playable(&g, "other", &sp).is_err());
        assert!(ensure_playable(&sp, "g1", &sp).is_err(), "no marker");
        // the singleplayer save folder around the co-op one
        assert!(ensure_playable(&g, "g1", &root).is_err());
        assert!(ensure_playable(&g, "g1", &g).is_err());
    }

    #[test]
    fn the_stamp_comes_from_the_file_or_from_the_files() {
        let d = tmp("stamp");
        assert_eq!(own_stamp(&d), Stamp::default());
        fs::create_dir_all(d.join("account").join("k")).unwrap();
        fs::write(d.join("account").join("k").join("f"), "x").unwrap();
        let s = own_stamp(&d);
        assert_eq!(s.seq, 0);
        assert!(s.saved_at > 0);
        fs::write(d.join(STAMP), r#"{"seq":7,"saved_at":123,"by":"m"}"#).unwrap();
        assert_eq!(own_stamp(&d), Stamp { seq: 7, saved_at: 123 });
        fs::write(d.join(STAMP), "junk").unwrap();
        assert_eq!(own_stamp(&d).seq, 0, "a stamp that does not read falls back to the file times");
    }

    #[test]
    fn peers_are_read_and_a_bad_file_is_empty() {
        let d = tmp("peers");
        assert!(peer_stamps(&d).is_empty());
        fs::write(d.join(PEERS), r#"{"peers":{"b":{"seq":3,"saved_at":10}}}"#).unwrap();
        assert_eq!(peer_stamps(&d)["b"], Stamp { seq: 3, saved_at: 10 });
        fs::write(d.join(PEERS), "{").unwrap();
        assert!(peer_stamps(&d).is_empty());
    }

    #[test]
    fn the_newest_copy_hosts() {
        let m = ids(&["a", "b", "c"]);
        let mut peers = HashMap::new();
        peers.insert("b".to_string(), Stamp { seq: 5, saved_at: 50 });
        peers.insert("c".to_string(), Stamp { seq: 4, saved_at: 99 });
        // a is behind b: a visits b
        let c = choose_host(&m, "a", Stamp { seq: 4, saved_at: 60 }, &peers);
        assert_eq!((c.host.as_str(), c.role, c.peers_known), ("b", Role::Visit, true));
        // a has the newest copy: a hosts
        let c = choose_host(&m, "a", Stamp { seq: 6, saved_at: 1 }, &peers);
        assert_eq!((c.host.as_str(), c.role), ("a", Role::Host));
        // seq first, then time
        peers.insert("a".to_string(), Stamp { seq: 4, saved_at: 60 });
        let c = choose_host(&m, "c", Stamp { seq: 5, saved_at: 60 }, &peers);
        assert_eq!((c.host.as_str(), c.role), ("c", Role::Host));
    }

    #[test]
    fn player_one_always_hosts_a_split_save() {
        let root = tmp("split-host");
        let p = crate::coop::create_split(&root, "Island", &ids(&["Alpha", "Beta", "Gamma"])).unwrap();
        ensure_split_folders(&root, &p).unwrap();
        // another player's copy is newer: player 1 still hosts
        fs::write(split_folder_of(&root, &p.id, &p.members[2].id).join(STAMP), r#"{"seq":9,"saved_at":99}"#).unwrap();
        fs::write(folder_of(&root, &p.id).join(STAMP), r#"{"seq":2,"saved_at":10}"#).unwrap();
        let c = host_of(&root, &p);
        assert_eq!((c.host.as_str(), c.role, c.own), (p.members[0].id.as_str(), Role::Host, Stamp { seq: 2, saved_at: 10 }));
    }

    #[test]
    fn each_split_copy_is_marked_and_keeps_its_files() {
        let root = tmp("split-folders");
        let p = crate::coop::create_split(&root, "Island", &ids(&["Alpha", "Beta", "Gamma"])).unwrap();
        let first = folder_of(&root, &p.id);
        let second = first.join("split").join(&p.members[1].id);
        let third = first.join("split").join(&p.members[2].id);
        assert_eq!(split_folder_of(&root, &p.id, &p.members[1].id), second);
        assert!(!second.exists());
        ensure_split_folders(&root, &p).unwrap();
        for dir in [&first, &second, &third] {
            assert_eq!(marker_group(dir).as_deref(), Some(p.id.as_str()));
            assert!(refuse_as_singleplayer(dir).is_err());
        }
        fs::write(second.join(STAMP), r#"{"seq":3,"saved_at":9}"#).unwrap();
        ensure_split_folders(&root, &p).unwrap();
        assert_eq!(own_stamp(&second), Stamp { seq: 3, saved_at: 9 });
        assert!(!split_folder_of(&root, &p.id, &p.me).exists());
        write_marker(&third, "other-group").unwrap();
        assert!(ensure_split_folders(&root, &p).is_err());
        assert_eq!(marker_group(&third).as_deref(), Some("other-group"));
    }

    #[test]
    fn a_tie_goes_to_the_first_member_on_every_pc() {
        let m = ids(&["a", "b", "c"]);
        let same = Stamp { seq: 2, saved_at: 20 };
        let peers: HashMap<_, _> = [("a", same), ("b", same), ("c", same)].into_iter().map(|(k, v)| (k.to_string(), v)).collect();
        for me in ["a", "b", "c"] {
            assert_eq!(choose_host(&m, me, same, &peers).host, "a", "as {}", me);
        }
        // nothing known at all (the first session): the creator hosts, the others visit
        let none = HashMap::new();
        assert_eq!(choose_host(&m, "a", Stamp::default(), &none).role, Role::Host);
        assert_eq!(choose_host(&m, "b", Stamp::default(), &none).role, Role::Visit);
        assert!(!choose_host(&m, "b", Stamp::default(), &none).peers_known);
    }

    #[test]
    fn the_player_can_override_the_automatic_role() {
        let m = ids(&["a", "b"]);
        let c = choose_host(&m, "b", Stamp::default(), &HashMap::new());
        assert_eq!(role_for(&c, StartAs::Auto), Role::Visit);
        assert_eq!(role_for(&c, StartAs::Host), Role::Host);
        assert_eq!(role_for(&c, StartAs::Visit), Role::Visit);
        assert_eq!(serde_json::from_str::<StartAs>(r#""visit""#).unwrap(), StartAs::Visit);
    }

    #[test]
    fn with_a_stamp_unknown_the_creator_hosts_on_every_pc() {
        // both have played into the group alone, nothing written peers.json: each PC once picked itself
        let m = ids(&["a", "b"]);
        let none = HashMap::new();
        assert_eq!(choose_host(&m, "a", Stamp { seq: 0, saved_at: 100 }, &none).role, Role::Host);
        assert_eq!(choose_host(&m, "b", Stamp { seq: 0, saved_at: 200 }, &none).role, Role::Visit);
        // one of three stamps unknown: still the creator
        let m = ids(&["a", "b", "c"]);
        let some: HashMap<_, _> = [("b".to_string(), Stamp { seq: 9, saved_at: 9 })].into_iter().collect();
        assert_eq!(choose_host(&m, "c", Stamp { seq: 9, saved_at: 99 }, &some).host, "a");
    }
}
