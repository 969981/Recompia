// SPDX-License-Identifier: GPL-3.0-or-later
//! "Save logs for a bug report": one zip a player attaches to an issue. What goes in (every file optional, a missing one is skipped):
//!
//!   logs/pkp.log, logs/pkp.log.prev       the game's log and the one before it (the last 4 MiB of each)
//!   logs/pkp_crash.txt, _freeze, _fatal   the game's crash, freeze and fatal reports
//!   logs/setup.log                        the launcher's setup log
//!   settings/pkp.ini                      the game's settings, secrets blanked
//!   coop-saves.json                       each co-op save's group id, player slots, host choice and relay: no room code, no invite, no ids
//!   system.txt                            launcher version, OS, CPU, RAM, GPUs
//!
//! Never in it: save data, launcher.json (the online salt), a co-op room code or invite. Room codes and invites are also cut out of the
//! log text, where the profile's own room code is known literally and any `XXXX-XXXX-XXXX-XXXX` or `PKCOOP1.` token is matched by shape.

use serde_json::{json, Value};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::coop::{self, Profile};
use crate::coop_save;
use crate::zipout::ZipOut;

/// The most of one file that goes in: its end, where the failure is.
pub const MAX_FILE: u64 = 4 << 20;
const REDACTED: &str = "[removed]";
/// Ini keys whose value is blanked (substring of the lower-case key).
const SECRET_KEYS: [&str; 7] = ["password", "token", "secret", "invite", "room_code", "passphrase", "salt"];
/// What the launcher opens from the game folder as reports (play.rs REPORT_FILES), plus the previous log.
const LOGS: [&str; 5] = ["pkp.log", "pkp.log.prev", "pkp_crash.txt", "pkp_freeze.txt", "pkp_fatal.txt"];

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

fn is_room_code(run: &str) -> bool {
    let parts: Vec<&str> = run.split('-').collect();
    parts.len() == 4 && parts.iter().all(|p| p.len() == 4 && p.bytes().all(|b| b.is_ascii_digit() || b.is_ascii_uppercase()))
}

/// `text` without the secrets: each of `secrets` (a known room code), `PKCOOP1.` invites and room-code shaped tokens.
pub fn redact(text: &str, secrets: &[String]) -> String {
    let mut s = text.to_string();
    for secret in secrets.iter().filter(|x| !x.is_empty()) {
        s = s.replace(secret.as_str(), REDACTED);
    }
    let mut out = String::with_capacity(s.len());
    let mut run = String::new();
    let flush = |run: &mut String, out: &mut String| {
        if run.starts_with("PKCOOP1.") || is_room_code(run) {
            out.push_str(REDACTED);
        } else {
            out.push_str(run);
        }
        run.clear();
    };
    for c in s.chars() {
        if c.is_ascii_alphanumeric() || c == '-' || c == '.' || c == '_' {
            run.push(c);
        } else {
            flush(&mut run, &mut out);
            out.push(c);
        }
    }
    flush(&mut run, &mut out);
    out
}

/// An ini with the value of every secret-looking key blanked. Comments and layout stay.
pub fn redact_ini(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for line in text.split_inclusive('\n') {
        let key = line.split('=').next().unwrap_or("").trim().to_ascii_lowercase();
        let secret = line.contains('=') && !key.starts_with(';') && !key.starts_with('#') && SECRET_KEYS.iter().any(|k| key.contains(k));
        if secret {
            let eol = &line[line.trim_end_matches(['\r', '\n']).len()..];
            out.push_str(&format!("{} = {}{}", line.split('=').next().unwrap_or("").trim_end(), REDACTED, eol));
        } else {
            out.push_str(line);
        }
    }
    out
}

fn save_card(root: &Path, p: &Profile) -> Value {
    let dir = coop_save::folder_of(root, &p.id);
    let host = coop_save::choose_host(&p.member_ids(), &p.me, coop_save::own_stamp(&dir), &coop_save::peer_stamps(&dir));
    let slot_of = |id: &str| p.members.iter().position(|m| m.id == id).map(|i| i + 1);
    json!({
        "group": p.id,
        "name": p.name,
        "relay": p.relay,
        "players": p.members.iter().enumerate().map(|(i, m)| json!({ "slot": i + 1, "name": m.name, "creator": i == 0, "this_pc": m.id == p.me })).collect::<Vec<_>>(),
        "host_choice": { "host_slot": slot_of(&host.host), "this_pc_role": host.role.word(), "peers_known": host.peers_known },
        "own_copy": { "seq": host.own.seq, "saved_at": host.own.saved_at },
    })
}

/// Everything about the co-op saves of `root` that is safe to share.
pub fn coop_cards(root: &Path) -> Value {
    Value::Array(coop::list(root).iter().map(|p| save_card(root, p)).collect())
}

/// The finished zip. `system` is the text of system.txt. Fails only when nothing at all could be gathered.
pub fn build(game_dir: &Path, coop_root: &Path, system: &str) -> Result<Vec<u8>, String> {
    let secrets: Vec<String> = coop::list(coop_root).into_iter().map(|p| p.room_code).collect();
    let mut zip = ZipOut::new();
    let mut count = 0;
    for name in LOGS.iter().copied().chain(std::iter::once(crate::setuplog::FILE)) {
        if let Some(text) = tail_of(&game_dir.join(name)) {
            zip.add(&format!("logs/{}", name), redact(&text, &secrets).as_bytes())?;
            count += 1;
        }
    }
    if let Some(text) = tail_of(&game_dir.join(crate::settings::PKP_INI)) {
        zip.add("settings/pkp.ini", redact(&redact_ini(&text), &secrets).as_bytes())?;
        count += 1;
    }
    let cards = coop_cards(coop_root);
    if cards.as_array().is_some_and(|a| !a.is_empty()) {
        zip.add("coop-saves.json", serde_json::to_string_pretty(&cards).map_err(|e| e.to_string())?.as_bytes())?;
        count += 1;
    }
    zip.add("system.txt", system.as_bytes())?;
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

    #[test]
    fn secrets_are_cut_out_of_text_and_ini() {
        let t = redact("join ABCD-0123-WXYZ-4567 and PKCOOP1.abc_DEF-1 ok; room 9999-8888-7777-6666!", &["9999-8888-7777-6666".into()]);
        assert_eq!(t, "join [removed] and [removed] ok; room [removed]!");
        assert_eq!(redact("frame 12-34 v1.2.3", &[]), "frame 12-34 v1.2.3");
        let ini = redact_ini("[net]\r\nroom_code = ABCD\r\nrelay = 192.0.2.1\r\n; password = x\r\npassword=hunter2\r\n");
        assert_eq!(ini, "[net]\r\nroom_code = [removed]\r\nrelay = 192.0.2.1\r\n; password = x\r\npassword = [removed]\r\n");
    }

    #[test]
    fn the_bundle_has_the_logs_and_no_secrets_or_saves() {
        let game = temp("game");
        let root = temp("root");
        fs::write(game.join("pkp.log"), "boot\nroom ABCD-EFGH-JKMN-PQRS\n").unwrap();
        fs::write(game.join("pkp.ini"), "[net]\nroom_code = ABCD-EFGH-JKMN-PQRS\nrelay = 192.0.2.1\n").unwrap();
        fs::create_dir_all(game.join("saves")).unwrap();
        fs::write(game.join("saves").join("slot.dat"), "SAVEDATA").unwrap();
        let p = coop::create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        let zip = build(&game, &root, "launcher 1.0\n").unwrap();
        let files = read_all(&zip);
        let names: Vec<&str> = files.iter().map(|(n, _)| n.as_str()).collect();
        assert_eq!(names, ["logs/pkp.log", "settings/pkp.ini", "coop-saves.json", "system.txt"]);
        let all: String = files.iter().map(|(_, d)| String::from_utf8_lossy(d).into_owned()).collect();
        assert!(!all.contains("ABCD-EFGH-JKMN-PQRS") && !all.contains(&p.room_code) && !all.contains("SAVEDATA"));
        let card = String::from_utf8_lossy(&files[2].1).into_owned();
        assert!(card.contains(&p.id) && card.contains("192.0.2.10") && card.contains("Ash") && !card.contains(&p.account_uid));
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
        assert!(build(&empty, &empty, "x").is_err());
        let _ = (fs::remove_dir_all(&game), fs::remove_dir_all(&empty));
    }
}
