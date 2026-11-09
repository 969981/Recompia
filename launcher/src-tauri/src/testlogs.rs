// SPDX-License-Identifier: GPL-3.0-or-later
//! The Testing tab's logs: list the files a tester is asked to send, read the end of one (for the viewer in the launcher), and
//! export them as one zip a tester can drag into a GitHub issue.
//!
//! Nothing is sent from here: the launcher has no GitHub login and must not carry a token, so "upload" means a zip in
//! `<game folder>/testing-logs/` plus the issue page opened in the browser. Every file is redacted first (user names in paths,
//! account and server settings, IP addresses) and cut to its last few MB, so the zip stays under GitHub's 25 MB attachment limit.
//! The zip is written by hand (stored, no compression): no new crate, and the format is tiny.

use serde::Serialize;
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// The folder next to pkp.exe that holds the exported zips.
pub const OUT_DIR: &str = "testing-logs";
/// How many exported zips are kept (the newest).
const KEEP_ZIPS: usize = 5;

/// What the Testing tab can show and export, with how many bytes of the end of each are kept in a zip.
const FILES: [(&str, u64); 8] = [
    ("pkp.log", 6 << 20),
    ("pkp.log.prev", 3 << 20),
    ("pkp_crash.txt", 1 << 20),
    ("pkp_freeze.txt", 1 << 20),
    ("pkp_fatal.txt", 1 << 20),
    ("setup.log", 3 << 20),
    ("setup.log.prev", 1 << 20),
    ("pkp.ini", 1 << 20),
];
/// The viewer reads at most this much from the end of a file.
const VIEW_BYTES: u64 = 2 << 20;
const VIEW_LINES: usize = 4000;

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct LogFile {
    pub name: String,
    pub bytes: u64,
    /// Unix seconds of the last write, when the system says.
    pub modified: Option<u64>,
}

#[derive(Serialize, Debug)]
pub struct Tail {
    pub name: String,
    /// The last lines of the file, redacted.
    pub text: String,
    /// True when the file has more before these lines.
    pub truncated: bool,
    pub bytes: u64,
}

#[derive(Serialize, Debug)]
pub struct Exported {
    pub path: String,
    pub dir: String,
    pub files: Vec<String>,
    pub bytes: u64,
}

fn cap_of(name: &str) -> Option<u64> {
    FILES.iter().find(|(n, _)| *n == name).map(|(_, c)| *c)
}

/// The files of the list that exist in the game folder, in the list's order.
pub fn list(game_dir: &Path) -> Vec<LogFile> {
    FILES
        .iter()
        .filter_map(|(name, _)| {
            let meta = fs::metadata(game_dir.join(name)).ok().filter(|m| m.is_file())?;
            let modified = meta.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs());
            Some(LogFile { name: (*name).to_string(), bytes: meta.len(), modified })
        })
        .collect()
}

/// The last `max_bytes` of a file as text, and whether anything came before. A cut in the middle of a line drops that line.
fn read_end(path: &Path, max_bytes: u64) -> Result<(String, bool, u64), String> {
    let mut f = fs::File::open(path).map_err(|e| format!("Cannot open {}: {}", path.display(), e))?;
    let len = f.metadata().map_err(|e| e.to_string())?.len();
    let start = len.saturating_sub(max_bytes);
    f.seek(SeekFrom::Start(start)).map_err(|e| e.to_string())?;
    let mut buf = Vec::with_capacity((len - start) as usize);
    f.take(max_bytes).read_to_end(&mut buf).map_err(|e| e.to_string())?;
    let mut text = String::from_utf8_lossy(&buf).into_owned();
    if start > 0 {
        text = match text.find('\n') {
            Some(i) => text[i + 1..].to_string(),
            None => String::new(),
        };
    }
    Ok((text, start > 0, len))
}

/// The end of one of the listed files for the viewer: at most `lines` lines (default and ceiling 4000), redacted.
pub fn read_tail(game_dir: &Path, name: &str, lines: Option<usize>) -> Result<Tail, String> {
    if cap_of(name).is_none() {
        return Err("That file cannot be shown from here.".into());
    }
    let path = game_dir.join(name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    let (text, mut truncated, bytes) = read_end(&path, VIEW_BYTES)?;
    let want = lines.unwrap_or(VIEW_LINES).clamp(1, VIEW_LINES);
    let all: Vec<&str> = text.lines().collect();
    let skip = all.len().saturating_sub(want);
    if skip > 0 {
        truncated = true;
    }
    let shown = all[skip..].join("\n");
    Ok(Tail { name: name.to_string(), text: redact(&shown, name.ends_with(".ini")), truncated, bytes })
}

// ---- redaction ---------------------------------------------------------------------------------------------------------------------

/// Ini keys whose value is about the person or their network, not the game: never leave the PC.
const SECRET_KEYS: [&str; 12] = ["uid", "nickname", "server", "token", "password", "passphrase", "secret", "address", "relay", "email", "host", "invite"];

fn secret_key(key: &str) -> bool {
    let k = key.trim().to_ascii_lowercase();
    SECRET_KEYS.iter().any(|s| k == *s || k.ends_with(&format!("_{}", s)))
}

/// `C:\Users\Name\x`, `/home/name/x` and `/Users/name/x` become `...\Users\<user>\x`, whatever the user is called.
fn mask_user_dirs(text: &str) -> String {
    const MARKERS: [&str; 3] = ["\\users\\", "/users/", "/home/"];
    let lower = text.to_ascii_lowercase();   // same byte offsets as text
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    while i < text.len() {
        if let Some(m) = MARKERS.iter().find(|m| lower[i..].starts_with(**m)) {
            let after = i + m.len();
            let end = text[after..].find(|c: char| matches!(c, '\\' | '/' | '"' | '\'') || c.is_whitespace()).map_or(text.len(), |n| after + n);
            out.push_str(&text[i..after]);
            if end > after {
                out.push_str("<user>");
            }
            i = end;
        } else {
            let ch = text[i..].chars().next().unwrap_or_default();
            out.push(ch);
            i += ch.len_utf8().max(1);
        }
    }
    out
}

/// A run of digits and dots that is a dotted quad up to 255 (not 127.x or 0.x) becomes `x.x.x.x`. Versions such as 10.0.19045.3693 stay.
fn mask_ipv4(text: &str) -> String {
    let mask = |run: &str| -> String {
        let core = run.trim_end_matches('.');
        let parts: Vec<&str> = core.split('.').collect();
        let quad = parts.len() == 4 && parts.iter().all(|p| (1..=3).contains(&p.len()) && p.parse::<u8>().is_ok());
        if quad && !matches!(parts[0], "127" | "0") { format!("x.x.x.x{}", &run[core.len()..]) } else { run.to_string() }
    };
    let mut out = String::with_capacity(text.len());
    let mut run = String::new();
    for c in text.chars().chain(std::iter::once('\0')) {
        if c.is_ascii_digit() || c == '.' {
            run.push(c);
            continue;
        }
        out.push_str(&mask(&run));
        run.clear();
        if c != '\0' {
            out.push(c);
        }
    }
    out
}

/// Takes the person out of a log or an ini: names in paths, secret ini values, IP addresses.
pub fn redact(text: &str, ini: bool) -> String {
    let masked = mask_ipv4(&mask_user_dirs(text));
    if !ini {
        return masked;
    }
    masked
        .lines()
        .map(|line| match line.split_once('=') {
            Some((key, value)) if secret_key(key) && !value.trim().is_empty() => format!("{}= <removed>", key),
            _ => line.to_string(),
        })
        .collect::<Vec<_>>()
        .join("\n")
}

// ---- zip ---------------------------------------------------------------------------------------------------------------------------

const CRC_TABLE: [u32; 256] = {
    let mut t = [0u32; 256];
    let mut n = 0;
    while n < 256 {
        let mut c = n as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[n] = c;
        n += 1;
    }
    t
};

pub fn crc32(data: &[u8]) -> u32 {
    !data.iter().fold(!0u32, |c, b| CRC_TABLE[((c ^ *b as u32) & 0xFF) as usize] ^ (c >> 8))
}

/// Little-endian writer for the zip headers.
#[derive(Default)]
struct Le(Vec<u8>);

impl Le {
    fn u16(&mut self, n: u16) -> &mut Self {
        self.0.extend_from_slice(&n.to_le_bytes());
        self
    }
    fn u32(&mut self, n: u32) -> &mut Self {
        self.0.extend_from_slice(&n.to_le_bytes());
        self
    }
}

/// A zip of stored (uncompressed) UTF-8-named entries; every size stays below 4 GiB.
pub fn zip(entries: &[(String, Vec<u8>)], when: chrono::DateTime<chrono::Utc>) -> Vec<u8> {
    use chrono::{Datelike, Timelike};
    let date = (((when.year().clamp(1980, 2107) - 1980) as u16) << 9) | ((when.month() as u16) << 5) | when.day() as u16;
    let time = ((when.hour() as u16) << 11) | ((when.minute() as u16) << 5) | (when.second() / 2) as u16;
    let (mut out, mut dir) = (Le::default(), Le::default());
    for (name, data) in entries {
        let (offset, crc, size, name_len) = (out.0.len() as u32, crc32(data), data.len() as u32, name.len() as u16);
        // flags 0x0800: the name is UTF-8; method 0: stored
        out.u32(0x0403_4b50).u16(20).u16(0x0800).u16(0).u16(time).u16(date).u32(crc).u32(size).u32(size).u16(name_len).u16(0);
        out.0.extend_from_slice(name.as_bytes());
        out.0.extend_from_slice(data);
        dir.u32(0x0201_4b50).u16(20).u16(20).u16(0x0800).u16(0).u16(time).u16(date).u32(crc).u32(size).u32(size).u16(name_len);
        dir.u16(0).u16(0).u16(0).u16(0).u32(0).u32(offset);
        dir.0.extend_from_slice(name.as_bytes());
    }
    let (start, count) = (out.0.len() as u32, entries.len() as u16);
    out.0.extend_from_slice(&dir.0);
    out.u32(0x0605_4b50).u16(0).u16(0).u16(count).u16(count).u32(dir.0.len() as u32).u32(start).u16(0);
    out.0
}

// ---- export ------------------------------------------------------------------------------------------------------------------------

fn safe_part(s: &str) -> String {
    let t: String = s.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '_' }).take(40).collect();
    if t.is_empty() { "unknown".into() } else { t }
}

fn report_text(version: &str, channel: &str, when: chrono::DateTime<chrono::Utc>, included: &[(String, bool)]) -> String {
    let mut s = String::new();
    s.push_str("Pokopia Recomp: Testing logs\n");
    s.push_str(&format!("Exported: {} UTC\n", when.format("%Y-%m-%d %H:%M:%S")));
    s.push_str(&format!("Launcher/game version: {}\n", version));
    s.push_str(&format!("Update channel: {}\n", if channel.is_empty() { "stable" } else { channel }));
    s.push_str(&format!("System: {} {}\n\n", std::env::consts::OS, std::env::consts::ARCH));
    s.push_str("Files (names in paths, account and server settings and IP addresses were removed; long files keep only their end):\n");
    for (name, cut) in included {
        s.push_str(&format!("  {}{}\n", name, if *cut { "  (end of file only)" } else { "" }));
    }
    s
}

/// Writes the redacted zip to `<game_dir>/testing-logs/` and keeps the newest few. Err with words when there is nothing to send.
pub fn export(game_dir: &Path, version: &str, channel: &str) -> Result<Exported, String> {
    let when = chrono::Utc::now();
    let mut entries: Vec<(String, Vec<u8>)> = Vec::new();
    let mut included: Vec<(String, bool)> = Vec::new();
    for (name, cap) in FILES {
        let path = game_dir.join(name);
        if !path.is_file() {
            continue;
        }
        let (text, cut, _) = read_end(&path, cap)?;
        entries.push((name.to_string(), redact(&text, name.ends_with(".ini")).into_bytes()));
        included.push((name.to_string(), cut));
    }
    if !entries.iter().any(|(n, _)| n != "pkp.ini") {
        return Err("There are no logs yet. Start the game once (or run Set up), then export again.".into());
    }
    entries.insert(0, ("report.txt".to_string(), report_text(version, channel, when, &included).into_bytes()));
    let dir = game_dir.join(OUT_DIR);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let file = dir.join(format!("PokopiaTesting-logs-{}-{}.zip", safe_part(version), when.format("%Y%m%d-%H%M%S")));
    let bytes = zip(&entries, when);
    fs::write(&file, &bytes).map_err(|e| format!("Cannot write {}: {}", file.display(), e))?;
    prune(&dir);
    Ok(Exported {
        path: file.to_string_lossy().into_owned(),
        dir: dir.to_string_lossy().into_owned(),
        files: entries.into_iter().map(|(n, _)| n).collect(),
        bytes: bytes.len() as u64,
    })
}

/// Deletes the oldest exported zips beyond the newest KEEP_ZIPS (only files this module names).
fn prune(dir: &Path) {
    let mut zips: Vec<PathBuf> = fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("PokopiaTesting-logs-") && n.ends_with(".zip")))
        .collect();
    zips.sort();   // the stamp in the name sorts oldest first
    while zips.len() > KEEP_ZIPS {
        let _ = fs::remove_file(zips.remove(0));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-testlogs-{}-{}", tag, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    /// Reads a stored zip back: (name, bytes) per entry, checking every crc and the directory's count.
    fn unzip(z: &[u8]) -> Vec<(String, Vec<u8>)> {
        let u16at = |i: usize| u16::from_le_bytes([z[i], z[i + 1]]) as usize;
        let u32at = |i: usize| u32::from_le_bytes([z[i], z[i + 1], z[i + 2], z[i + 3]]);
        let eocd = z.len() - 22;
        assert_eq!(u32at(eocd), 0x0605_4b50, "end of central directory");
        let count = u16at(eocd + 10);
        let mut at = u32at(eocd + 16) as usize;
        let mut out = Vec::new();
        for _ in 0..count {
            assert_eq!(u32at(at), 0x0201_4b50, "central entry");
            let crc = u32at(at + 16);
            let size = u32at(at + 24) as usize;
            let name_len = u16at(at + 28);
            let local = u32at(at + 42) as usize;
            let name = String::from_utf8(z[at + 46..at + 46 + name_len].to_vec()).unwrap();
            assert_eq!(u32at(local), 0x0403_4b50, "local header");
            let data_at = local + 30 + u16at(local + 26) + u16at(local + 28);
            let data = z[data_at..data_at + size].to_vec();
            assert_eq!(crc32(&data), crc, "crc of {}", name);
            out.push((name, data));
            at += 46 + name_len;
        }
        assert_eq!(at, eocd, "the directory ends where the end record starts");
        out
    }

    #[test]
    fn crc32_matches_the_standard_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn zip_roundtrips_and_python_accepts_it() {
        let entries = vec![("a.txt".to_string(), b"hello\n".to_vec()), ("dir é.txt".to_string(), vec![0u8; 5000]), ("empty".to_string(), Vec::new())];
        let z = zip(&entries, chrono::Utc::now());
        assert_eq!(unzip(&z), entries);
        let d = temp("py");
        fs::write(d.join("t.zip"), &z).unwrap();
        // an independent reader, when the machine has one
        if let Ok(o) = std::process::Command::new("python3").args(["-I", "-c", "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; print('PYZIP', len(z.namelist()))"]).arg(d.join("t.zip")).output() {
            if o.status.success() {
                assert!(String::from_utf8_lossy(&o.stdout).contains("PYZIP 3"));
            }
        }
        let _ = fs::remove_dir_all(d);
    }

    #[test]
    fn paths_keep_their_shape() {
        assert_eq!(mask_user_dirs(r"C:\Users\Alice\Games\x"), r"C:\Users\<user>\Games\x");
        assert_eq!(mask_user_dirs("/home/alice/GitHub/x and /home/alice"), "/home/<user>/GitHub/x and /home/<user>");
        assert_eq!(mask_user_dirs("/Users/Bob/Library"), "/Users/<user>/Library");
        assert_eq!(mask_user_dirs("c:\\USERS\\Zed\\x"), "c:\\USERS\\<user>\\x");
        assert_eq!(mask_user_dirs("/home/<user>/x"), "/home/<user>/x");
        assert_eq!(mask_user_dirs("nothing to hide: /usr/lib é"), "nothing to hide: /usr/lib é");
    }

    #[test]
    fn ip_addresses_go_but_versions_stay() {
        assert_eq!(mask_ipv4("peer 192.168.1.20:47610 ok"), "peer x.x.x.x:47610 ok");
        assert_eq!(mask_ipv4("relay 8.8.4.4."), "relay x.x.x.x.");
        assert_eq!(mask_ipv4("Windows 10.0.19045.3693 driver 552.44 vk 1.3.280 127.0.0.1 0.0.0.0"), "Windows 10.0.19045.3693 driver 552.44 vk 1.3.280 127.0.0.1 0.0.0.0");
        assert_eq!(mask_ipv4("1.2.3.4.5 999.1.1.1 1..2.3"), "1.2.3.4.5 999.1.1.1 1..2.3");
    }

    #[test]
    fn ini_secrets_are_removed_but_game_settings_stay() {
        let ini = "[net]\nuid = abcd1234\nserver = my.host.example:47610\n[account]\nnickname = Alice\n[pkp]\ncamera_aim = 1\nmouse_mode_key = F9\nuid =\n";
        let r = redact(ini, true);
        assert!(r.contains("uid = <removed>") && r.contains("server = <removed>") && r.contains("nickname = <removed>"), "{r}");
        assert!(!r.contains("abcd1234") && !r.contains("my.host.example") && !r.contains("Alice"));
        assert!(r.contains("camera_aim = 1") && r.contains("mouse_mode_key = F9"));
        assert!(r.ends_with("uid ="), "an empty secret stays empty: {r}");
    }

    #[test]
    fn viewer_refuses_other_files_and_tails_big_ones() {
        let d = temp("view");
        assert!(read_tail(&d, "../secret.txt", None).is_err());
        assert!(read_tail(&d, "pkp.exe", None).is_err());
        assert!(read_tail(&d, "pkp.log", None).unwrap_err().contains("does not exist"));
        let body: String = (0..10000).map(|i| format!("line {i}\n")).collect();
        fs::write(d.join("pkp.log"), &body).unwrap();
        let t = read_tail(&d, "pkp.log", Some(5)).unwrap();
        assert_eq!(t.text, "line 9995\nline 9996\nline 9997\nline 9998\nline 9999");
        assert!(t.truncated);
        let all = read_tail(&d, "pkp.log", Some(1_000_000)).unwrap();
        assert_eq!(all.text.lines().count(), VIEW_LINES, "the ceiling holds");
        let _ = fs::remove_dir_all(d);
    }

    #[test]
    fn export_says_so_when_there_is_nothing() {
        let d = temp("none");
        assert!(export(&d, "0.9.1-beta.1", "testing").unwrap_err().contains("no logs yet"));
        fs::write(d.join("pkp.ini"), "[pkp]\n").unwrap();
        assert!(export(&d, "0.9.1-beta.1", "testing").is_err(), "an ini alone is not a log");
        let _ = fs::remove_dir_all(d);
    }

    #[test]
    fn export_makes_a_valid_redacted_zip() {
        let d = temp("export");
        let home = r"C:\Users\Alice\Games";
        fs::write(d.join("pkp.log"), format!("start in {home}\npeer 192.168.0.9 joined\nGPU: NVIDIA 552.44\n")).unwrap();
        fs::write(d.join("setup.log"), "setup ok\n").unwrap();
        fs::write(d.join("pkp.ini"), "[net]\nuid = SECRETUID\n[pkp]\ncamera_aim = 1\n").unwrap();
        fs::write(d.join("pkp.exe"), "not a log").unwrap();
        let ex = export(&d, "0.9.1-beta.1", "testing").unwrap();
        assert!(ex.path.contains("testing-logs") && ex.path.ends_with(".zip"));
        assert_eq!(ex.files, vec!["report.txt", "pkp.log", "setup.log", "pkp.ini"]);
        let entries = unzip(&fs::read(&ex.path).unwrap());
        let get = |n: &str| String::from_utf8(entries.iter().find(|(m, _)| m == n).unwrap().1.clone()).unwrap();
        let log = get("pkp.log");
        assert!(log.contains(r"C:\Users\<user>\Games") && log.contains("peer x.x.x.x joined") && log.contains("NVIDIA 552.44"));
        assert!(!log.contains("Alice") && !log.contains("192.168"));
        assert!(!get("pkp.ini").contains("SECRETUID") && get("pkp.ini").contains("camera_aim = 1"));
        let report = get("report.txt");
        assert!(report.contains("0.9.1-beta.1") && report.contains("testing") && report.contains("pkp.log"));
        assert!(!entries.iter().any(|(n, _)| n == "pkp.exe"));
        assert!(ex.bytes > 0 && ex.bytes == fs::metadata(&ex.path).unwrap().len());
        let _ = fs::remove_dir_all(d);
    }

    #[test]
    fn only_the_newest_zips_are_kept() {
        let d = temp("prune");
        for i in 0..8 {
            fs::write(d.join(format!("PokopiaTesting-logs-v-2026010{i}-000000.zip")), "x").unwrap();
        }
        fs::write(d.join("keep-me.txt"), "x").unwrap();
        prune(&d);
        let mut names: Vec<String> = fs::read_dir(&d).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        names.sort();
        assert_eq!(names.len(), KEEP_ZIPS + 1);
        assert!(names.contains(&"keep-me.txt".to_string()) && !names.iter().any(|n| n.contains("20260100")));
        let _ = fs::remove_dir_all(d);
    }
}
