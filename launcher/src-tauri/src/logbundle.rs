// SPDX-License-Identifier: GPL-3.0-or-later
//! "Save logs for a bug report": one zip a player attaches to a public issue. What goes in (every file optional, a missing one is skipped):
//!
//!   logs/pkp.log, logs/pkp.log.prev..4    the game's logs of the last five launches, newest first (the last 8 MiB of each)
//!   logs/pkp_crash.txt, _freeze, _fatal   the game's crash, freeze and fatal reports
//!   logs/setup.log                        the launcher's setup log
//!   settings/pkp.ini                      the game's settings, private values blanked (SECRET_KEYS, commented-out lines too)
//!   coop-saves.json                       per co-op save: a hash of its group id, its name, the players as Player 1..4 with their slots,
//!                                         the host choice, and whether it plays on the public relay, a custom relay, or without one
//!   system.txt                            launcher version, OS, CPU, RAM, GPUs
//!
//! Never in it: save data, launcher.json (the online salt), profiles.json, coop.json. The logs, the ini, system.txt and the save names
//! in coop-saves.json are also passed through `redact`, with what this PC knows to be private (`Private::of`): co-op room codes, account and player uids (and the account folder names made
//! from them), group ids (as the same hash coop-saves.json shows), the home folder (as `~`), the OS user name and the players' names
//! (whole words), plus anything shaped like a room code, a `PKCOOP1.` invite or an IPv4 or IPv6 address.

use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::coop::{self, Profile};
use crate::zipout::ZipOut;
use crate::{coop_save, profiles, saveimport};

/// The most of one file that goes in: its end, where the failure is.
pub const MAX_FILE: u64 = 8 << 20;
const REDACTED: &str = "[removed]";
const IP: &str = "[ip]";
const UID: &str = "[uid]";
const USER: &str = "[user]";
const PLAYER: &str = "[player]";
/// Ini keys whose value is blanked (substring of the lower-case key, also on a commented-out line), and `uid` or `*_uid` (not `uuid_repair`).
const SECRET_KEYS: [&str; 12] =
    ["password", "token", "secret", "invite", "room_code", "passphrase", "salt", "nickname", "server", "relay", "private_room", "account_id"];
/// Names shorter than this are not cut out of the text: as whole words they would take ordinary words with them.
const MIN_WORD: usize = 3;
/// What the launcher opens from the game folder as reports (play.rs REPORT_FILES), plus the logs of the four launches before
/// (the game keeps five: nxrt log.h `LogOptions::keep_launches`).
const LOGS: [&str; 8] =
    ["pkp.log", "pkp.log.prev", "pkp.log.prev2", "pkp.log.prev3", "pkp.log.prev4", "pkp_crash.txt", "pkp_freeze.txt", "pkp_fatal.txt"];

/// What a bundle must not give away. `Private::of` gathers it from this PC; tests make their own.
#[derive(Default)]
pub struct Private {
    /// Literal text (any case) and what stands in for it: room codes, uids, group ids.
    pub swap: Vec<(String, String)>,
    /// Home folders, replaced (any case, either slash, JSON-escaped too) by `~`.
    pub homes: Vec<String>,
    /// Whole words (any case) and what stands in for them: the OS user name, the players' names.
    pub words: Vec<(String, String)>,
}

/// The hash coop-saves.json shows for group id `id`; the logs show the same one in its place.
pub fn group_tag(id: &str) -> String {
    let h = Sha256::digest(format!("recompia log bundle group\0{}", id).as_bytes());
    format!("group-{}", h[..4].iter().map(|b| format!("{:02x}", b)).collect::<String>())
}

impl Private {
    /// Everything this PC knows that a public bug report must not show: the co-op saves of `coop_root`, the profiles of `game_dir` and the
    /// OS user.
    pub fn of(game_dir: &Path, coop_root: &Path) -> Private {
        let mut p = Private::default();
        let uid = |p: &mut Private, u: &str| {
            p.swap.push((u.to_string(), UID.into()));
            if let Some(k) = saveimport::uid_key(u) {
                p.swap.push((k, UID.into())); // the account folder name the runtime makes from it
            }
        };
        for s in coop::list(coop_root) {
            p.swap.push((s.room_code.clone(), REDACTED.into()));
            p.swap.push((s.id.clone(), group_tag(&s.id)));
            uid(&mut p, &s.account_uid);
            for m in &s.members {
                uid(&mut p, &m.id);
                p.words.push((m.name.clone(), PLAYER.into()));
                if s.split {
                    let nickname = profiles::nickname_of(&m.name);
                    if nickname != m.name {
                        p.words.push((nickname, PLAYER.into()));
                    }
                }
            }
        }
        for prof in profiles::load(game_dir).profiles {
            uid(&mut p, &prof.uid);
            uid(&mut p, &prof.net_uid);
            if prof.nickname != profiles::DEFAULT_NICKNAME {
                p.words.push((prof.nickname.clone(), PLAYER.into())); // the default is nobody's name, and a word the logs use
            }
        }
        for k in ["USERPROFILE", "HOME"] {
            if let Ok(h) = std::env::var(k) {
                if let Some(name) = Path::new(&h).file_name().and_then(|n| n.to_str()) {
                    user_word(&mut p, name);
                }
                p.homes.push(h);
            }
        }
        for k in ["USERNAME", "USER", "LOGNAME"] {
            if let Ok(u) = std::env::var(k) {
                user_word(&mut p, &u);
            }
        }
        p
    }
}

/// System user names every machine of a kind shares (SteamOS, Proton's prefix, Linux): nobody's name, and words the logs need
/// ("Steam Deck", the Proton prefix path).
const GENERIC_USERS: [&str; 5] = ["deck", "steamuser", "root", "user", "admin"];

fn user_word(p: &mut Private, name: &str) {
    if !GENERIC_USERS.iter().any(|g| g.eq_ignore_ascii_case(name)) {
        p.words.push((name.to_string(), USER.into()));
    }
}

/// The end of `path` (at most `MAX_FILE` bytes, from a line start) as text; None when it is missing or empty.
fn tail_of(path: &Path) -> Option<String> {
    let mut f = File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    let cut = len > MAX_FILE;
    if cut {
        f.seek(SeekFrom::Start(len - MAX_FILE)).ok()?;
    }
    let mut bytes = Vec::new();
    f.read_to_end(&mut bytes).ok()?;
    let mut text = String::from_utf8_lossy(&bytes).into_owned();
    if text.is_empty() {
        return None;
    }
    if cut {
        text = match text.find('\n') {
            Some(i) => text[i + 1..].to_string(),
            None => text,
        };
        text.insert_str(0, "[earlier lines left out]\n");
    }
    Some(text)
}

/// `hay` with every `needle` replaced by `with`, ASCII case ignored; `word`: only where no letter or digit touches it on either side.
fn replace_ci(hay: &str, needle: &str, with: &str, word: bool) -> String {
    if needle.is_empty() {
        return hay.to_string();
    }
    // ASCII lower-casing keeps every byte where it was, so positions in `low` are positions in `hay`
    let low = hay.to_ascii_lowercase();
    let n = needle.to_ascii_lowercase();
    let is_word = |c: Option<char>| c.is_some_and(|c| c.is_alphanumeric());
    let mut out = String::with_capacity(hay.len());
    let mut at = 0;
    let mut from = 0;
    while let Some(i) = low[from..].find(&n).map(|i| i + from) {
        let end = i + n.len();
        if word && (is_word(hay[..i].chars().next_back()) || is_word(hay[end..].chars().next())) {
            from = i + low[i..].chars().next().map_or(1, |c| c.len_utf8());
            continue;
        }
        out.push_str(&hay[at..i]);
        out.push_str(with);
        at = end;
        from = end;
    }
    out.push_str(&hay[at..]);
    out
}

fn is_room_code(run: &str) -> bool {
    let parts: Vec<&str> = run.split('-').collect();
    parts.len() == 4 && parts.iter().all(|p| p.len() == 4 && p.bytes().all(|b| b.is_ascii_digit() || b.is_ascii_uppercase()))
}

fn is_ipv4(s: &str) -> bool {
    let parts: Vec<&str> = s.split('.').collect();
    parts.len() == 4 && parts.iter().all(|p| (1..=3).contains(&p.len()) && p.bytes().all(|b| b.is_ascii_digit()) && p.parse::<u16>().is_ok_and(|v| v <= 255))
}

/// A run of name characters (letters, digits, `-`, `.`, `_`) without what it must not show: a `PKCOOP1.` invite whole; else each piece
/// between `_` that is an IPv4 address (a trailing `.` aside), and each piece between `.` and `_` that is a room code.
fn clean_run(run: &str, out: &mut String) {
    if run.starts_with("PKCOOP1.") {
        out.push_str(REDACTED);
        return;
    }
    for (i, piece) in run.split('_').enumerate() {
        if i > 0 {
            out.push('_');
        }
        let core = piece.trim_end_matches('.');
        if is_ipv4(core) {
            out.push_str(IP);
            out.push_str(&piece[core.len()..]);
            continue;
        }
        for (j, part) in piece.split('.').enumerate() {
            if j > 0 {
                out.push('.');
            }
            out.push_str(if is_room_code(part) { REDACTED } else { part });
        }
    }
}

/// `run` (letters, digits, `:`, `.`, `%`) when it is an IPv6 address with at least two groups written and a digit (so `::1` and names
/// like `add::face` stay), a zone or a trailing `.` aside, or one trailing `:` (`<address>: timed out`).
fn is_ipv6(run: &str) -> bool {
    let core = run.trim_end_matches('.');
    let core = core.split('%').next().unwrap_or(core);
    let addr = |s: &str| {
        s.matches(':').count() >= 2 && s.parse::<std::net::Ipv6Addr>().is_ok() && s.split(':').filter(|g| !g.is_empty()).count() >= 2
            && s.bytes().any(|b| b.is_ascii_digit())
    };
    addr(core) || (core.ends_with(':') && !core.ends_with("::") && addr(&core[..core.len() - 1]))
}

/// `run` with an IPv6 address in it (is_ipv6) replaced, a trailing `:` or `.` kept.
fn clean_ipv6(run: &str, out: &mut String) {
    if !is_ipv6(run) {
        return out.push_str(run);
    }
    out.push_str(IP);
    let core = run.trim_end_matches('.');
    if core.ends_with(':') && !core.ends_with("::") {
        out.push(':');
    }
    out.push_str(&run[core.len()..]);
}

/// Each run of `text` whose characters pass `keep` given to `f`; the characters between runs are kept.
fn by_runs(text: &str, keep: impl Fn(char) -> bool, mut f: impl FnMut(&str, &mut String)) -> String {
    let mut out = String::with_capacity(text.len());
    let mut start = None;
    for (i, c) in text.char_indices() {
        match (keep(c), start) {
            (true, None) => start = Some(i),
            (false, Some(s)) => {
                f(&text[s..i], &mut out);
                start = None;
                out.push(c);
            }
            (false, None) => out.push(c),
            (true, Some(_)) => {}
        }
    }
    if let Some(s) = start {
        f(&text[s..], &mut out);
    }
    out
}

/// `text` without what `private` names, and without anything shaped like a room code, an invite or an IP address.
pub fn redact(text: &str, private: &Private) -> String {
    let mut s = text.to_string();
    let mut homes: Vec<String> = private
        .homes
        .iter()
        .filter(|h| h.trim_end_matches(['/', '\\']).len() > 1)
        .flat_map(|h| {
            let h = h.trim_end_matches(['/', '\\']);
            [h.to_string(), h.replace('\\', "/"), h.replace('/', "\\"), h.replace('/', "\\").replace('\\', "\\\\")]
        })
        .collect();
    homes.sort_by_key(|h| std::cmp::Reverse(h.len()));
    for h in &homes {
        s = replace_ci(&s, h, "~", false);
    }
    for (needle, with) in private.swap.iter().filter(|(n, _)| n.len() >= 8) {
        s = replace_ci(&s, needle, with, false);
    }
    for (word, with) in private.words.iter().filter(|(w, _)| w.trim().chars().count() >= MIN_WORD) {
        s = replace_ci(&s, word.trim(), with, true);
    }
    let s = by_runs(&s, |c| c.is_ascii_alphanumeric() || matches!(c, ':' | '.' | '%'), clean_ipv6);
    by_runs(&s, |c| c.is_ascii_alphanumeric() || matches!(c, '-' | '.' | '_'), clean_run)
}

/// An ini with the value of every private key blanked, on commented-out lines (`; password = x`, `# token=`) too. Comments and layout stay.
pub fn redact_ini(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for line in text.split_inclusive('\n') {
        let body = line.trim_start().trim_start_matches([';', '#']).trim_start();
        let lead = &line[..line.len() - body.len()];
        let key = body.split('=').next().unwrap_or("").trim().to_ascii_lowercase();
        if body.contains('=') && (SECRET_KEYS.iter().any(|k| key.contains(k)) || key == "uid" || key.ends_with("_uid")) {
            let eol = &line[line.trim_end_matches(['\r', '\n']).len()..];
            out.push_str(&format!("{}{} = {}{}", lead, body.split('=').next().unwrap_or("").trim_end(), REDACTED, eol));
        } else {
            out.push_str(line);
        }
    }
    out
}

/// "public" when `relay` is the launcher's public relay (`public_relay`, the address the page uses), else "custom".
fn relay_kind(relay: &str, public_relay: &str) -> &'static str {
    let addr = crate::relay::invite_client::addr;
    if addr(relay).is_some() && addr(relay) == addr(public_relay) {
        "public"
    } else {
        "custom"
    }
}

/// One co-op save as coop-saves.json shows it. Its name is the one free text, redacted on its own (not the whole file: a name like `true`
/// or `name` would rewrite its JSON).
fn save_card(root: &Path, p: &Profile, public_relay: &str, private: &Private) -> Value {
    let host = coop_save::host_of(root, p);
    let slot_of = |id: &str| p.members.iter().position(|m| m.id == id).map(|i| i + 1);
    // In split screen, this_pc_role and own_copy describe window 1.
    json!({
        "group": group_tag(&p.id),
        "name": redact(&p.name, private),
        "relay": if p.split { "none" } else { relay_kind(&p.relay, public_relay) },
        "players": p.members.iter().enumerate().map(|(i, m)| json!({ "slot": i + 1, "name": format!("Player {}", i + 1), "creator": i == 0, "this_pc": p.split || m.id == p.me })).collect::<Vec<_>>(),
        "host_choice": { "host_slot": slot_of(&host.host), "this_pc_role": host.role.word(), "peers_known": host.peers_known },
        "own_copy": { "seq": host.own.seq, "saved_at": host.own.saved_at },
    })
}

/// Everything about the co-op saves of `root` that is safe to share.
pub fn coop_cards(root: &Path, public_relay: &str, private: &Private) -> Value {
    Value::Array(coop::list(root).iter().map(|p| save_card(root, p, public_relay, private)).collect())
}

/// The finished zip. `system` is the text of system.txt; `public_relay` the launcher's public relay. Fails only when nothing at all could
/// be gathered.
pub fn build(game_dir: &Path, coop_root: &Path, system: &str, public_relay: &str) -> Result<Vec<u8>, String> {
    let private = Private::of(game_dir, coop_root);
    let mut zip = ZipOut::new();
    let mut count = 0;
    for name in LOGS.iter().copied().chain(std::iter::once(crate::setuplog::FILE)) {
        if let Some(text) = tail_of(&game_dir.join(name)) {
            zip.add(&format!("logs/{}", name), redact(&text, &private).as_bytes())?;
            count += 1;
        }
    }
    if let Some(text) = tail_of(&game_dir.join(crate::settings::PKP_INI)) {
        zip.add("settings/pkp.ini", redact(&redact_ini(&text), &private).as_bytes())?;
        count += 1;
    }
    let cards = coop_cards(coop_root, public_relay, &private);
    if cards.as_array().is_some_and(|a| !a.is_empty()) {
        zip.add("coop-saves.json", serde_json::to_string_pretty(&cards).map_err(|e| e.to_string())?.as_bytes())?;
        count += 1;
    }
    zip.add("system.txt", redact(system, &private).as_bytes())?;
    if count == 0 {
        return Err("There are no logs yet. Start the game once, then try again.".into());
    }
    Ok(zip.finish())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::zipout::read_all;
    use std::fs;

    fn temp(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-logbundle-{}-{}", tag, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn private() -> Private {
        Private {
            swap: vec![("9999-8888-7777-6666".into(), REDACTED.into()), ("0123456789abcdef0123456789abcdef".into(), UID.into()), ("fedcba9876543210".into(), group_tag("fedcba9876543210"))],
            homes: vec!["C:\\Users\\Testuser".into(), "/home/testuser".into()],
            words: vec![("Testuser".into(), USER.into()), ("Misty".into(), PLAYER.into()), ("Al".into(), PLAYER.into())],
        }
    }

    #[test]
    fn room_codes_and_invites_are_cut_out_by_shape() {
        let p = Private::default();
        let t = redact("join ABCD-0123-WXYZ-4567 and PKCOOP1.abc_DEF-1 ok; room 9999-8888-7777-6666!", &private());
        assert_eq!(t, "join [removed] and [removed] ok; room [removed]!");
        assert_eq!(redact("frame 12-34 v1.2.3", &p), "frame 12-34 v1.2.3");
        // a room code followed by `.` or `_`, or after `_`
        assert_eq!(redact("code ABCD-0123-WXYZ-4567. next", &p), "code [removed]. next");
        assert_eq!(redact("ABCD-0123-WXYZ-4567_x room_ABCD-0123-WXYZ-4567", &p), "[removed]_x room_[removed]");
    }

    #[test]
    fn addresses_are_cut_out_and_look_alikes_stay() {
        let p = Private::default();
        assert_eq!(redact("relay 192.0.2.1:47610, then 198.51.100.7.", &p), "relay [ip]:47610, then [ip].");
        assert_eq!(redact("peer=203.0.113.250_1 at 10:00:00.5", &p), "peer=[ip]_1 at 10:00:00.5");
        assert_eq!(redact("from [2001:db8::17]:47610 and fe80::1%12 or ::ffff:192.0.2.9", &p), "from [[ip]]:47610 and [ip] or [ip]");
        assert_eq!(redact("2001:db8:0:0:1:2:3:4 x", &p), "[ip] x");
        assert_eq!(redact("connect to 2001:db8::1: timed out, then 2001:db8::2.", &p), "connect to [ip]: timed out, then [ip].");
        // not addresses
        for keep in ["pkp::title::open", "12:34:56", "::1 loopback", "v1.2.3", "300.1.2.3", "1.2.3", "std::fs add::face"] {
            assert_eq!(redact(keep, &p), keep);
        }
    }

    #[test]
    fn home_user_ids_and_names_are_cut_out() {
        let t = redact(
            "save C:\\Users\\Testuser\\Saves and c:/users/testuser/x; json \"C:\\\\Users\\\\Testuser\\\\a\"; Z:\\home\\testuser\\g; /mnt/c/Users/TESTUSER/p",
            &private(),
        );
        assert_eq!(t, "save ~\\Saves and ~/x; json \"~\\\\a\"; Z:~\\g; /mnt/c/Users/[user]/p");
        let t = redact("uid 0123456789ABCDEF0123456789ABCDEF group fedcba9876543210 player Misty joined; Mistyland, flash, Al, also", &private());
        assert_eq!(t, format!("uid [uid] group {} player [player] joined; Mistyland, flash, Al, also", group_tag("fedcba9876543210")));
        assert!(group_tag("fedcba9876543210").starts_with("group-") && group_tag("x") != group_tag("y"));
    }

    #[test]
    fn private_ini_values_are_blanked_on_commented_lines_too() {
        let ini = redact_ini(
            "[net]\r\nroom_code = ABCD\r\nrelay = 192.0.2.1\r\n; password = x\r\n# token=\r\npassword=hunter2\r\nserver = auto\r\nuid = 0123\r\nprivate_room = 1\r\n; coop_uid=9\r\n[account]\r\nnickname = Misty\r\nnetwork_account_id = 0011\r\nmode = auto\r\nuuid_repair = 1\r\n; a comment\r\n",
        );
        assert_eq!(
            ini,
            "[net]\r\nroom_code = [removed]\r\nrelay = [removed]\r\n; password = [removed]\r\n# token = [removed]\r\npassword = [removed]\r\nserver = [removed]\r\nuid = [removed]\r\nprivate_room = [removed]\r\n; coop_uid = [removed]\r\n[account]\r\nnickname = [removed]\r\nnetwork_account_id = [removed]\r\nmode = auto\r\nuuid_repair = 1\r\n; a comment\r\n"
        );
    }

    #[test]
    fn the_bundle_has_the_logs_and_no_secrets_or_saves() {
        let game = temp("game");
        let root = temp("root");
        let p = coop::create(&root, "Island", "Misty", "192.0.2.10").unwrap();
        let q = coop::create(&root, "Cave", "Misty", "192.0.2.11:47610").unwrap();
        let member = &p.members[0].id;
        let own = "00112233445566778899aabbccddeeff"; // the profile's [account] uid, and the account folder the runtime names after it
        let folder = saveimport::uid_key(own).unwrap();
        fs::write(game.join("pkp.log"), format!("boot\nroom ABCD-EFGH-JKMN-PQRS\nplayer Misty uid {} group {} acct {}\nsave {} {}\n", member, p.id, p.account_uid, own, folder)).unwrap();
        fs::write(game.join("pkp.ini"), format!("[net]\nroom_code = ABCD-EFGH-JKMN-PQRS\nrelay = 192.0.2.1\n[account]\nuid = {}\n", own)).unwrap();
        fs::create_dir_all(game.join("saves")).unwrap();
        fs::write(game.join("saves").join("slot.dat"), "SAVEDATA").unwrap();
        let zip = build(&game, &root, "launcher 1.0\n", "192.0.2.10:47610").unwrap();
        let files = read_all(&zip);
        let names: Vec<&str> = files.iter().map(|(n, _)| n.as_str()).collect();
        assert_eq!(names, ["logs/pkp.log", "settings/pkp.ini", "coop-saves.json", "system.txt"]);
        let all: String = files.iter().map(|(_, d)| String::from_utf8_lossy(d).into_owned()).collect();
        for secret in ["ABCD-EFGH-JKMN-PQRS", &p.room_code, "SAVEDATA", "Misty", &p.id, member, &p.account_uid, "192.0.2.", own, &folder] {
            assert!(!all.contains(secret), "{}", secret);
        }
        let log = String::from_utf8_lossy(&files[0].1).into_owned();
        assert!(log.contains(&format!("player [player] uid [uid] group {} acct [uid]\nsave [uid] [uid]", group_tag(&p.id))), "{}", log);
        let cards: Value = serde_json::from_slice(&files[2].1).unwrap();
        let card = |id: &str| cards.as_array().unwrap().iter().find(|c| c["group"] == group_tag(id)).unwrap().clone();
        assert_eq!(card(&p.id)["relay"], "public");
        assert_eq!(card(&q.id)["relay"], "custom");
        assert_eq!(card(&p.id)["players"][0]["name"], "Player 1");
        let _ = (fs::remove_dir_all(&game), fs::remove_dir_all(&root));
    }

    #[test]
    fn a_split_save_card_reports_local_players_and_the_host_slot() {
        let game = temp("split-game");
        let root = temp("split-root");
        let p = coop::create_split(&root, "Local world", &["Alpha".into(), "Beta".into()]).unwrap();
        coop_save::ensure_split_folders(&root, &p).unwrap();
        let second = &p.members[1].id;
        let copy = coop_save::split_folder_of(&root, &p.id, second);
        fs::write(copy.join(coop_save::STAMP), r#"{"seq":2,"saved_at":42}"#).unwrap();
        let private = Private::of(&game, &root);
        let cards = coop_cards(&root, "192.0.2.10", &private);
        let card = &cards[0];
        assert_eq!(card["relay"], "none");
        assert_eq!(card["host_choice"]["host_slot"], 1, "player 1 hosts a split save, a newer copy or not");
        assert_eq!(card["players"][0]["this_pc"], true);
        assert_eq!(card["players"][1]["this_pc"], true);
        for secret in [&p.room_code, &p.account_uid, &p.id, second, &p.members[0].name, &p.members[1].name] {
            assert!(!card.to_string().contains(secret));
            assert!(!redact(secret, &private).contains(secret));
        }
        let _ = (fs::remove_dir_all(&game), fs::remove_dir_all(&root));
    }

    #[test]
    fn a_big_log_keeps_its_end_and_nothing_means_an_error() {
        let game = temp("big");
        let line = "0123456789abcdef\n";
        fs::write(game.join("pkp.log"), line.repeat((MAX_FILE as usize / line.len()) + 100) + "LAST\n").unwrap();
        let t = tail_of(&game.join("pkp.log")).unwrap();
        assert!(t.starts_with("[earlier lines left out]\n0123") && t.ends_with("LAST\n") && t.len() <= MAX_FILE as usize + 64);
        let empty = temp("none");
        assert!(build(&empty, &empty, "x", "192.0.2.10").is_err());
        let _ = (fs::remove_dir_all(&game), fs::remove_dir_all(&empty));
    }
}
