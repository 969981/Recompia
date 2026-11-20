// SPDX-License-Identifier: GPL-3.0-or-later
//! Co-op profiles: a co-op save with its members, played only from Recompia: Co-op (never as singleplayer; coop_save.rs has the folder contract).
//!
//! A profile is a folder of its own, `<root>/<id>/`, where `root` is the `coop-saves` folder beside the singleplayer save folder (so
//! the two never nest). The profile lives in that folder's `coop.json`, next to the save it describes, so the folder can be copied to
//! another PC as it is. Nothing in profiles.json or pkp.ini changes: a co-op start hands the game everything through its environment
//! (coop_launch.rs), so a singleplayer start after it finds no trace.
//!
//! Members are a stable id (32 hex digits, the member's Link Play uid too) and a display name. The first member is the creator, who
//! hosts the first session. One shared account uid names the account folder in every member's copy, so the copies are interchangeable.
//! An online save has a private room on its relay. The creator adds each player by name and sends the invite with the room code.
//! A player joins as the name the creator gave them; a newer invite brings an existing copy's member list up to date.
//! A split screen save starts with all its players on the creator's PC, each with their own copy. It has no relay or invite.
//! Only the creator changes the member list in either mode.

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

use crate::coop_save;
use crate::profiles::{clean_name, new_uid};

pub const FILE: &str = "coop.json";
pub const ROOT_NAME: &str = "coop-saves";
pub const MAX_MEMBERS: usize = 4;
const VERSION: u32 = 1;
/// What an invite starts with, so a pasted text is recognised (and the format can change).
const INVITE_PREFIX: &str = "PKCOOP1.";
/// Crockford's alphabet (no I, L, O, U): what the game's room codes use (runtime/docs/CONFIG.md [net] room_code).
const ROOM_ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Member {
    /// 32 hex digits: the member's id and its Link Play uid.
    pub id: String,
    pub name: String,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Profile {
    pub version: u32,
    /// The folder name and the group id the game gets.
    pub id: String,
    pub name: String,
    /// [account] uid of the shared save, the same in every member's copy.
    pub account_uid: String,
    /// The private room's code on the relay: `XXXX-XXXX-XXXX-XXXX`. A secret of the members.
    pub room_code: String,
    /// `IPv4[:port]` of the relay that holds the room; empty for split screen.
    pub relay: String,
    /// Creator first.
    pub members: Vec<Member>,
    /// The member who plays on this PC (window 1 for split screen).
    pub me: String,
    /// All members play on this PC.
    #[serde(default)]
    pub split: bool,
}

impl Profile {
    /// This PC plays the creator (the first member), who alone changes the member list.
    pub fn is_creator(&self) -> bool {
        self.members.first().is_some_and(|m| m.id == self.me)
    }
    pub fn member(&self, id: &str) -> Option<&Member> {
        self.members.iter().find(|m| m.id == id)
    }
    pub fn member_ids(&self) -> Vec<String> {
        self.members.iter().map(|m| m.id.clone()).collect()
    }
}

// ---- where ---------------------------------------------------------------------------------------------------------------------------

/// The folder of all co-op saves: beside the singleplayer save folder.
pub fn root_beside(singleplayer_save_dir: &Path) -> PathBuf {
    singleplayer_save_dir.parent().unwrap_or(singleplayer_save_dir).join(ROOT_NAME)
}

/// The root for this install (saveimport::locate names the singleplayer save folder).
pub fn root_of(game_dir: &Path) -> PathBuf {
    root_beside(&crate::saveimport::locate(game_dir).0)
}

// ---- storing -------------------------------------------------------------------------------------------------------------------------

fn read(dir: &Path) -> Option<Profile> {
    let p: Profile = serde_json::from_str(&fs::read_to_string(dir.join(FILE)).ok()?).ok()?;
    // the marker decides what the folder is: a coop.json in a folder without it is not a profile
    if coop_save::marker_group(dir).as_deref() != Some(p.id.as_str()) || p.member(&p.me).is_none() {
        return None;
    }
    // Split member ids are also folder names, including on case-insensitive filesystems.
    if p.split {
        let valid_members = p.members.iter().enumerate().all(|(i, m)| {
            is_hex_id(&m.id, 32) && !p.members[..i].iter().any(|other| m.id.eq_ignore_ascii_case(&other.id))
        });
        if !is_hex_id(&p.id, 16) || !p.is_creator() || p.members.len() > MAX_MEMBERS || !valid_members {
            return None;
        }
    }
    Some(p)
}

fn write(root: &Path, p: &Profile) -> Result<(), String> {
    let text = serde_json::to_string_pretty(p).map_err(|e| e.to_string())?;
    crate::settings::write_atomic(&coop_save::folder_of(root, &p.id).join(FILE), &text)
}

/// Every co-op profile under `root`, by name. A folder that is not one (no marker, damaged coop.json) is left out and left alone.
pub fn list(root: &Path) -> Vec<Profile> {
    let mut v: Vec<Profile> = fs::read_dir(root).map(|rd| rd.flatten().filter_map(|e| read(&e.path())).collect()).unwrap_or_default();
    v.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()).then(a.id.cmp(&b.id)));
    v
}

pub fn get(root: &Path, id: &str) -> Result<Profile, String> {
    // the id becomes a folder name: only what this module made is accepted
    if id.is_empty() || !id.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("That co-op save does not exist.".into());
    }
    read(&coop_save::folder_of(root, id)).ok_or_else(|| "That co-op save does not exist.".into())
}

// ---- making --------------------------------------------------------------------------------------------------------------------------

/// 16 symbols of the room alphabet from the OS random source, as `XXXX-XXXX-XXXX-XXXX` (80 bits).
fn new_room_code() -> Result<String, String> {
    let mut b = [0u8; 16];
    crate::relay::random::os_random(&mut b).map_err(|e| format!("Cannot make a room code: {}", e))?;
    let s: Vec<char> = b.iter().map(|x| ROOM_ALPHABET[(*x & 31) as usize] as char).collect();
    Ok(s.chunks(4).map(|c| c.iter().collect::<String>()).collect::<Vec<_>>().join("-"))
}

fn room_code_ok(c: &str) -> bool {
    let parts: Vec<&str> = c.split('-').collect();
    parts.len() == 4 && parts.iter().all(|p| p.len() == 4 && p.bytes().all(|b| ROOM_ALPHABET.contains(&b)))
}

/// `IPv4[:port]` with a public or private unicast address: what the game's [net] relay takes (no host name).
pub fn relay_ok(s: &str) -> bool {
    let (ip, port) = match s.trim().split_once(':') {
        Some((i, p)) => (i, p.parse::<u32>().ok()),
        None => (s.trim(), Some(1)),
    };
    let o: Vec<u32> = ip.split('.').filter_map(|x| x.parse().ok()).collect();
    o.len() == 4 && ip.split('.').count() == 4 && o.iter().all(|x| *x <= 255) && o[0] != 0 && o[0] < 224 && port.is_some_and(|p| (1..=65535).contains(&p))
}

fn name_of_member(raw: &str) -> Result<String, String> {
    clean_name(raw).map_err(|_| "Type a name for the player (1 to 24 characters).".to_string())
}

fn name_of_save(raw: &str) -> Result<String, String> {
    clean_name(raw).map_err(|_| "Type a name for the co-op save (1 to 24 characters).".to_string())
}

fn new_member(name: String, members: &[Member]) -> Result<Member, String> {
    if members.iter().any(|m| m.name.eq_ignore_ascii_case(&name)) {
        return Err(format!("There is already a player called {}.", name));
    }
    let taken: Vec<&str> = members.iter().map(|m| m.id.as_str()).collect();
    Ok(Member { id: new_uid(&taken)?, name })
}

/// A new co-op save with its creator as the first member, in a new folder.
pub fn create(root: &Path, name: &str, my_name: &str, relay: &str) -> Result<Profile, String> {
    let name = name_of_save(name)?;
    let my_name = name_of_member(my_name)?;
    if !relay_ok(relay) {
        return Err("The relay needs an address like 192.0.2.10 or 192.0.2.10:47610.".into());
    }
    create_profile(root, name, vec![my_name], relay.trim().to_string(), false)
}

pub const SPLIT_PLAYER_COUNT: &str = "A split screen save needs 2 to 4 players.";

/// A new split screen save with all local players in window order.
pub fn create_split(root: &Path, name: &str, players: &[String]) -> Result<Profile, String> {
    let name = name_of_save(name)?;
    if !(2..=MAX_MEMBERS).contains(&players.len()) {
        return Err(SPLIT_PLAYER_COUNT.into());
    }
    let names = players.iter().map(|raw| name_of_member(raw)).collect::<Result<Vec<_>, _>>()?;
    create_profile(root, name, names, String::new(), true)
}

fn create_profile(root: &Path, name: String, names: Vec<String>, relay: String, split: bool) -> Result<Profile, String> {
    if list(root).iter().any(|p| p.name.eq_ignore_ascii_case(&name)) {
        return Err(format!("There is already a co-op save called {}.", name));
    }
    let mut members = Vec::with_capacity(names.len());
    for name in names {
        members.push(new_member(name, &members)?);
    }
    let me = members[0].id.clone();
    let taken: Vec<&str> = members.iter().map(|m| m.id.as_str()).collect();
    let p = Profile {
        version: VERSION,
        id: new_uid(&taken)?[..16].to_string(),
        name,
        account_uid: new_uid(&taken)?,
        room_code: new_room_code()?,
        relay,
        members,
        me,
        split,
    };
    coop_save::make_coop_folder(&coop_save::folder_of(root, &p.id), &p.id)?;
    write(root, &p)?;
    Ok(p)
}

const CREATOR_ONLY: &str = "Only the player who made this co-op save can change its players.";

/// A new member, with an id made here, so the invite can name them before they join. At most `MAX_MEMBERS`. Creator only.
pub fn add_member(root: &Path, id: &str, name: &str) -> Result<Profile, String> {
    let mut p = get(root, id)?;
    if !p.is_creator() {
        return Err(CREATOR_ONLY.into());
    }
    let name = name_of_member(name)?;
    if p.members.len() >= MAX_MEMBERS {
        return Err(format!("A co-op save has at most {} players.", MAX_MEMBERS));
    }
    p.members.push(new_member(name, &p.members)?);
    write(root, &p)?;
    Ok(p)
}

/// Drops a member. The creator (first) and the member who plays on this PC stay. Creator only.
pub fn remove_member(root: &Path, id: &str, member: &str) -> Result<Profile, String> {
    let mut p = get(root, id)?;
    if !p.is_creator() {
        return Err(CREATOR_ONLY.into());
    }
    if p.members.first().is_some_and(|m| m.id == member) || p.me == member {
        return Err("The creator and the player on this PC cannot be removed.".into());
    }
    let before = p.members.len();
    p.members.retain(|m| m.id != member);
    if p.members.len() == before {
        return Err("That player is not in this co-op save.".into());
    }
    write(root, &p)?;
    Ok(p)
}

// ---- invite and join -----------------------------------------------------------------------------------------------------------------

#[derive(Serialize, Deserialize)]
struct Invite {
    id: String,
    name: String,
    account_uid: String,
    room_code: String,
    relay: String,
    members: Vec<Member>,
}

const SPLIT_NO_INVITE: &str = "A split screen save is played on this PC only: it has no invite.";

/// The text the creator hands to the others. It carries the room code, so it goes to members only.
pub fn invite(root: &Path, id: &str) -> Result<String, String> {
    use base64::Engine;
    let p = get(root, id)?;
    if p.split {
        return Err(SPLIT_NO_INVITE.into());
    }
    let inv = Invite { id: p.id, name: p.name, account_uid: p.account_uid, room_code: p.room_code, relay: p.relay, members: p.members };
    let json = serde_json::to_vec(&inv).map_err(|e| e.to_string())?;
    Ok(format!("{}{}", INVITE_PREFIX, base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(json)))
}

fn is_hex_id(s: &str, n: usize) -> bool {
    s.len() == n && s.bytes().all(|b| b.is_ascii_hexdigit())
}

fn parse_invite(text: &str) -> Result<Invite, String> {
    use base64::Engine;
    let bad = || "This is not a co-op invite.".to_string();
    let body = text.trim().strip_prefix(INVITE_PREFIX).ok_or_else(bad)?;
    let bytes = base64::engine::general_purpose::URL_SAFE_NO_PAD.decode(body).map_err(|_| bad())?;
    let inv: Invite = serde_json::from_slice(&bytes).map_err(|_| bad())?;
    let ok = is_hex_id(&inv.id, 16)
        && is_hex_id(&inv.account_uid, 32)
        && room_code_ok(&inv.room_code)
        && relay_ok(&inv.relay)
        && !inv.members.is_empty()
        && inv.members.len() <= MAX_MEMBERS
        && inv.members.iter().all(|m| is_hex_id(&m.id, 32) && clean_name(&m.name).is_ok());
    ok.then_some(inv).ok_or_else(bad)
}

/// Joins a co-op save from its invite, as the player the creator added for you (`member`), in a new folder of this PC. The creator's own
/// place is not joined: that would be a second copy of the creator. When this PC has the save already, the invite only brings its name,
/// relay and players up to date (the creator added or removed one): this PC stays the player it was, and `member` is ignored.
pub fn accept(root: &Path, text: &str, member: &str) -> Result<Profile, String> {
    let inv = parse_invite(text)?;
    if let Some(mut have) = read(&coop_save::folder_of(root, &inv.id)) {
        if have.split {
            return Err(SPLIT_NO_INVITE.into());
        }
        if have.account_uid != inv.account_uid || have.room_code != inv.room_code {
            return Err("This invite does not match the co-op save you have.".into());
        }
        if have.is_creator() {
            return Err("You made this co-op save: send the invite to the other players.".into());
        }
        if !inv.members.iter().any(|m| m.id == have.me) {
            return Err("You are no longer a player in this co-op save.".into());
        }
        have.name = inv.name;
        have.relay = inv.relay;
        have.members = inv.members;
        write(root, &have)?;
        return Ok(have);
    }
    if inv.members.first().is_some_and(|m| m.id == member) || !inv.members.iter().any(|m| m.id == member) {
        return Err("That player is not in the invite.".into());
    }
    let p = Profile { version: VERSION, id: inv.id, name: inv.name, account_uid: inv.account_uid, room_code: inv.room_code, relay: inv.relay, members: inv.members, me: member.to_string(), split: false };
    coop_save::make_coop_folder(&coop_save::folder_of(root, &p.id), &p.id)?;
    write(root, &p)?;
    Ok(p)
}

/// Deletes co-op save `id` from this PC: its folder, with this PC's copy of the world. The other members keep their copies. Only a folder
/// this module made (a hex id with this group's marker) is removed.
pub fn delete(root: &Path, id: &str) -> Result<Profile, String> {
    let p = get(root, id)?;
    let dir = coop_save::folder_of(root, &p.id);
    if coop_save::marker_group(&dir).as_deref() != Some(p.id.as_str()) {
        return Err("That co-op save does not exist.".into());
    }
    fs::remove_dir_all(&dir).map_err(|e| format!("Cannot delete {}: {}", crate::dump::fwd(&dir), e))?;
    Ok(p)
}

/// The id of the co-op save an invite is for, so the page can tell a new save from an update of one it has. None: not an invite.
pub fn invite_save_id(text: &str) -> Option<String> {
    parse_invite(text).ok().map(|i| i.id)
}

/// The players an invite names, creator first, for the page to ask "who are you?". None: the text is not an invite.
pub fn invite_members(text: &str) -> Option<Vec<Member>> {
    parse_invite(text).ok().map(|i| i.members)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!("pkp-coop-{}-{}-{}", tag, std::process::id(), N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn a_co_op_save_gets_its_own_marked_folder_and_a_private_room() {
        let root = tmp("create");
        let p = create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        let dir = coop_save::folder_of(&root, &p.id);
        assert_eq!(coop_save::marker_group(&dir).as_deref(), Some(p.id.as_str()));
        assert!(room_code_ok(&p.room_code), "{}", p.room_code);
        assert_eq!(p.members.len(), 1);
        assert_eq!(p.me, p.members[0].id);
        assert_eq!(p.account_uid.len(), 32);
        assert_eq!(list(&root), vec![p.clone()]);
        // two saves never share a room, an account or a member id
        let q = create(&root, "Beach", "Ash", "192.0.2.10").unwrap();
        assert_ne!((&p.room_code, &p.account_uid, &p.me), (&q.room_code, &q.account_uid, &q.me));
        assert!(create(&root, "island", "Ash", "192.0.2.10").is_err());
    }

    #[test]
    fn names_and_relays_are_checked() {
        let root = tmp("checks");
        assert!(create(&root, "  ", "Ash", "192.0.2.10").is_err());
        assert!(create(&root, "A", "", "192.0.2.10").is_err());
        for bad in ["", "relay.example.org", "192.0.2.10:0", "192.0.2.10:70000", "0.1.2.3", "224.0.0.1", "1.2.3", "1.2.3.4.5"] {
            assert!(create(&root, "A", "Ash", bad).is_err(), "{}", bad);
        }
        assert!(create(&root, "A", "Ash", "127.0.0.1:47610").is_ok());
    }

    #[test]
    fn a_split_save_starts_with_all_local_players_and_has_no_invite() {
        let root = tmp("split");
        let p = create_split(&root, "  Island  ", &["  Alpha  ".into(), "Beta".into()]).unwrap();
        assert!(p.split && p.is_creator() && p.relay.is_empty());
        assert_eq!(p.name, "Island");
        assert_eq!(p.members.iter().map(|m| m.name.as_str()).collect::<Vec<_>>(), ["Alpha", "Beta"]);
        assert_ne!(p.members[0].id, p.members[1].id);
        assert_eq!(get(&root, &p.id).unwrap(), p);
        assert_eq!(invite(&root, &p.id).unwrap_err(), SPLIT_NO_INVITE);
        let added = add_member(&root, &p.id, "Gamma").unwrap();
        assert_eq!(added.members.len(), 3);
        assert_eq!(remove_member(&root, &p.id, &added.members[2].id).unwrap(), p);
    }

    #[test]
    fn invalid_split_players_leave_no_save() {
        let root = tmp("split-invalid");
        for names in [vec![], vec!["Alpha"], vec!["A", "B", "C", "D", "E"], vec!["Alpha", " alpha "], vec!["Alpha", ""], vec!["Alpha", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"]] {
            let players = names.into_iter().map(String::from).collect::<Vec<_>>();
            assert!(create_split(&root, "Island", &players).is_err());
        }
        assert!(list(&root).is_empty());
        assert!(fs::read_dir(root).unwrap().next().is_none());
    }

    #[test]
    fn existing_profiles_without_the_split_field_stay_online() {
        let root = tmp("old-profile");
        let p = create(&root, "Island", "Alpha", "192.0.2.10").unwrap();
        let path = coop_save::folder_of(&root, &p.id).join(FILE);
        let mut json: serde_json::Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        json.as_object_mut().unwrap().remove("split");
        fs::write(path, json.to_string()).unwrap();
        assert_eq!(get(&root, &p.id).unwrap(), p);
        assert!(!p.split);
    }

    #[test]
    fn members_are_added_and_removed_with_limits() {
        let root = tmp("members");
        let p = create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        let p = add_member(&root, &p.id, "Misty").unwrap();
        assert!(add_member(&root, &p.id, "misty").is_err());
        let p = add_member(&root, &p.id, "Brock").unwrap();
        let p = add_member(&root, &p.id, "Dawn").unwrap();
        assert!(add_member(&root, &p.id, "May").is_err(), "four at most");
        assert!(remove_member(&root, &p.id, &p.members[0].id).is_err(), "the creator stays");
        let brock = p.members[2].id.clone();
        let p = remove_member(&root, &p.id, &brock).unwrap();
        assert_eq!(p.members.len(), 3);
        assert!(remove_member(&root, &p.id, &brock).is_err());
        assert!(get(&root, "../x").is_err());
    }

    #[test]
    fn an_invite_makes_the_same_save_on_another_pc() {
        let (a, b) = (tmp("host"), tmp("guest"));
        let p = create(&a, "Island", "Ash", "192.0.2.10:47610").unwrap();
        let p = add_member(&a, &p.id, "Misty").unwrap();
        let text = invite(&a, &p.id).unwrap();
        assert_eq!(invite_members(&text).unwrap(), p.members);
        let misty = p.members[1].id.clone();
        let q = accept(&b, &text, &misty).unwrap();
        assert!(!q.split);
        assert_eq!((&q.id, &q.room_code, &q.account_uid, &q.relay, &q.members), (&p.id, &p.room_code, &p.account_uid, &p.relay, &p.members));
        assert_eq!(q.me, misty);
        assert_eq!(q.members[0], p.members[0], "the creator is first on every PC");
        // the same invite again changes nothing; one with a new player brings the list up to date, still as Misty
        assert_eq!(accept(&b, &text, "").unwrap().members.len(), 2);
        let p = add_member(&a, &p.id, "Brock").unwrap();
        let newer = invite(&a, &p.id).unwrap();
        let q = accept(&b, &newer, "").unwrap();
        assert_eq!((q.members.len(), q.me.as_str()), (3, misty.as_str()));
        assert!(add_member(&b, &q.id, "Dawn").is_err(), "only the creator adds players");
        assert!(accept(&a, &newer, "").is_err(), "the creator does not join their own save");
        let p = remove_member(&a, &p.id, &misty).unwrap();
        assert!(accept(&b, &invite(&a, &p.id).unwrap(), "").is_err(), "removed from the save");
        // not as the creator, not as a stranger
        assert!(accept(&tmp("c"), &text, &p.members[0].id).is_err());
        assert!(accept(&tmp("d"), &text, "ffff").is_err());
        assert_eq!(list(&b).len(), 1);
    }

    #[test]
    fn a_save_is_deleted_with_its_folder_and_nothing_else() {
        let root = tmp("delete");
        let p = create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        let q = create(&root, "Beach", "Ash", "192.0.2.10").unwrap();
        fs::create_dir_all(coop_save::folder_of(&root, &p.id).join("account").join("k")).unwrap();
        assert_eq!(delete(&root, &p.id).unwrap().id, p.id);
        assert!(!coop_save::folder_of(&root, &p.id).exists());
        assert_eq!(list(&root), vec![q]);
        assert!(delete(&root, &p.id).is_err());
        assert!(delete(&root, "..").is_err());
        // a folder without the marker is not a co-op save and stays
        let other = root.join("abcdef0123456789");
        fs::create_dir_all(&other).unwrap();
        assert!(delete(&root, "abcdef0123456789").is_err());
        assert!(other.exists());
    }

    #[test]
    fn a_text_that_is_not_an_invite_is_refused() {
        let root = tmp("badinvite");
        for t in ["", "hello", "PKCOOP1.", "PKCOOP1.!!!", "PKCOOP1.e30"] {
            assert!(accept(&root, t, "x").is_err(), "{}", t);
            assert!(invite_members(t).is_none(), "{}", t);
        }
    }

    #[test]
    fn a_folder_without_the_marker_is_not_a_profile() {
        let root = tmp("nomarker");
        let p = create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        fs::remove_file(coop_save::folder_of(&root, &p.id).join(coop_save::MARKER)).unwrap();
        assert!(list(&root).is_empty());
        assert!(get(&root, &p.id).is_err());
    }

    #[test]
    fn the_root_sits_beside_the_singleplayer_save() {
        let r = root_beside(Path::new("game").join("save").as_path());
        assert_eq!(r, Path::new("game").join(ROOT_NAME));
    }
}
