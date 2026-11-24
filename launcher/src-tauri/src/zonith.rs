// SPDX-License-Identifier: GPL-3.0-or-later
//! Zonith, the launcher's second game. The launcher keeps Zonith's own setup app up to date and opens it to build the game from the
//! player's files; once the game is built the launcher starts the game itself. This file is the pure part: the release list, which
//! release this platform installs, what the launcher downloaded (`<data>/zonith/current.json`), the game the setup built (`read_game`)
//! and the progress the page shows. The commands are zonith_cmds.rs.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Unauthenticated: until the repository is public this answers 404, which is "not public yet".
pub const RELEASES_URL: &str = "https://api.github.com/repos/Dittobyte/Zonith/releases?per_page=20";

/// The setup file this platform downloads (by the end of its name).
pub const ASSET_SUFFIX: &str = if cfg!(windows) { "-windows.zip" } else { ".AppImage" };

/// Zonith has Windows and Linux builds only.
pub const SUPPORTED: bool = cfg!(any(windows, target_os = "linux"));

/// What the launcher downloaded last, in `<data>/zonith/`.
pub const CURRENT_FILE: &str = "current.json";

/// The folder the Windows zip holds, with TotK-Setup.exe in it.
pub const WINDOWS_ROOT: &str = "TotK-Setup-windows";
pub const WINDOWS_EXE: &str = "TotK-Setup.exe";

#[derive(Deserialize, Clone, Debug)]
pub struct GhAsset {
    pub name: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub browser_download_url: String,
    /// "sha256:<hex>", which GitHub lists for assets
    #[serde(default)]
    pub digest: Option<String>,
}

#[derive(Deserialize, Clone, Debug)]
pub struct GhRelease {
    pub tag_name: String,
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default)]
    pub published_at: Option<String>,
    #[serde(default)]
    pub body: Option<String>,
    #[serde(default)]
    pub html_url: String,
    #[serde(default)]
    pub prerelease: bool,
    #[serde(default)]
    pub draft: bool,
    #[serde(default)]
    pub assets: Vec<GhAsset>,
}

impl GhRelease {
    fn title(&self) -> String {
        self.name.clone().filter(|n| !n.trim().is_empty()).unwrap_or_else(|| format!("Zonith {}", self.tag_name))
    }
}

/// A release for the What's New page.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct ReleaseRow {
    pub tag: String,
    pub name: String,
    pub date: String,
    pub body: String,
    pub url: String,
    pub prerelease: bool,
}

/// The release this platform installs, plus what the download needs (not sent).
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Latest {
    pub tag: String,
    pub name: String,
    pub date: String,
    pub url: String,
    /// the setup file's name
    pub asset: String,
    pub size: u64,
    #[serde(skip)]
    pub download_url: String,
    #[serde(skip)]
    pub sha256: String,
}

pub fn parse_releases(body: &[u8]) -> Result<Vec<GhRelease>, String> {
    let list: Vec<GhRelease> = serde_json::from_slice(body).map_err(|e| e.to_string())?;
    Ok(list.into_iter().filter(|r| !r.draft).collect())
}

/// The releases, newest first (by version; GitHub's order otherwise).
pub fn rows(list: &[GhRelease]) -> Vec<ReleaseRow> {
    let mut out: Vec<ReleaseRow> = list
        .iter()
        .map(|r| ReleaseRow { tag: r.tag_name.clone(), name: r.title(), date: r.published_at.clone().unwrap_or_default(), body: r.body.clone().unwrap_or_default(), url: r.html_url.clone(), prerelease: r.prerelease })
        .collect();
    out.sort_by(|a, b| match (crate::releases::Version::parse(&a.tag), crate::releases::Version::parse(&b.tag)) {
        (Some(x), Some(y)) => y.cmp(&x),
        _ => std::cmp::Ordering::Equal,
    });
    out
}

/// The lower-case hex of a GitHub "sha256:<hex>" digest; None for any other form.
pub fn parse_digest(d: &str) -> Option<String> {
    let hex = d.trim().strip_prefix("sha256:")?;
    (hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_hexdigit())).then(|| hex.to_ascii_lowercase())
}

/// The newest non-prerelease with this platform's setup and its digest (an asset without one cannot be checked, so it is skipped).
pub fn latest(list: &[GhRelease], suffix: &str) -> Option<Latest> {
    list.iter()
        .filter(|r| !r.prerelease && crate::releases::Version::parse(&r.tag_name).is_some())
        .filter_map(|r| {
            let a = r.assets.iter().find(|a| a.name.ends_with(suffix) && !a.browser_download_url.is_empty())?;
            let sha256 = parse_digest(a.digest.as_deref()?)?;
            Some(Latest {
                tag: r.tag_name.clone(),
                name: r.title(),
                date: r.published_at.clone().unwrap_or_default(),
                url: r.html_url.clone(),
                asset: a.name.clone(),
                size: a.size,
                download_url: a.browser_download_url.clone(),
                sha256,
            })
        })
        .max_by(|a, b| crate::releases::Version::parse(&a.tag).cmp(&crate::releases::Version::parse(&b.tag)))
}

/// The tag as a folder name.
pub fn safe_tag(tag: &str) -> String {
    let t: String = tag.chars().filter(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_')).collect();
    if t.is_empty() || t.chars().all(|c| c == '.') { "latest".into() } else { t }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Current {
    pub tag: String,
    /// the release file it came from (the AppImage on Linux, the zip on Windows)
    #[serde(default)]
    pub asset: String,
}

/// The setup app to start, in the launcher's Zonith folder: Windows `<tag>/TotK-Setup-windows/TotK-Setup.exe`; Linux the AppImage at the
/// folder's top, always at the same path, because the rebuild command Zonith writes into an install starts that file (docs/installer.md).
pub fn app_path(zonith: &Path, c: &Current) -> std::path::PathBuf {
    if cfg!(windows) { zonith.join(safe_tag(&c.tag)).join(WINDOWS_ROOT).join(WINDOWS_EXE) } else { zonith.join(&c.asset) }
}

/// What the launcher downloaded: current.json, when its setup app is still there.
pub fn read_current(zonith: &Path) -> Option<Current> {
    let c: Current = serde_json::from_str(&std::fs::read_to_string(zonith.join(CURRENT_FILE)).ok()?).ok()?;
    if c.tag.is_empty() {
        return None;
    }
    app_path(zonith, &c).is_file().then_some(c)
}

/// The release tag is newer than the downloaded one ("v0.5.1" against "0.5.0"; releases::Version drops a leading v).
pub fn update_available(installed: &str, tag: &str) -> bool {
    crate::releases::is_newer(tag, installed)
}

/// The `zonith-progress` payload.
#[derive(Serialize, Clone, Debug, Default, PartialEq)]
pub struct ZonithProgress {
    /// download | unpack | done | error | cancelled
    pub state: String,
    pub done: Option<u64>,
    pub total: Option<u64>,
    pub pct: Option<f64>,
    pub message: Option<String>,
}

const DOWNLOAD_SHARE: f64 = 90.0;

impl ZonithProgress {
    pub fn download(done: u64, total: u64) -> Self {
        let frac = if total > 0 { (done as f64 / total as f64).min(1.0) } else { 0.0 };
        ZonithProgress { state: "download".into(), done: Some(done), total: Some(total), pct: Some((DOWNLOAD_SHARE * frac * 10.0).round() / 10.0), message: None }
    }
    pub fn unpack() -> Self {
        ZonithProgress { state: "unpack".into(), pct: Some(DOWNLOAD_SHARE), ..Default::default() }
    }
    pub fn end(state: &str, message: Option<String>) -> Self {
        ZonithProgress { state: state.into(), pct: (state == "done").then_some(100.0), message, ..Default::default() }
    }
}

/// The file Zonith's setup writes at the top of every game folder it installs (docs/installer.md).
pub const INSTALL_MARKER: &str = "totk-install.json";

/// The game's file in the install folder.
pub const GAME_EXE: &str = if cfg!(windows) { "totk.exe" } else { "totk" };

/// The game Zonith's setup built, as its own files tell it (Zonith docs/installer.md, "Starting the game from another program").
#[derive(Clone, Debug, PartialEq)]
pub struct Game {
    /// the install folder
    pub dir: PathBuf,
    /// the Zonith release whose setup last finished on it ("0.5.1")
    pub release: String,
    /// the game is built: a setup run finished and the game's file is there
    pub ready: bool,
}

/// The file Zonith's setup records its last install folder in: `%LOCALAPPDATA%\TotK Recomp\setup.json` on Windows, elsewhere
/// `$XDG_STATE_HOME/totk-recomp/setup.json` (default `~/.local/state`). TOTK_SETUP_RECENT names another file, for the setup and for us.
pub fn recent_file() -> Option<PathBuf> {
    let var = |k: &str| std::env::var_os(k).filter(|v| !v.is_empty()).map(PathBuf::from);
    if let Some(p) = var("TOTK_SETUP_RECENT") {
        return Some(p);
    }
    if cfg!(windows) {
        return Some(var("LOCALAPPDATA")?.join("TotK Recomp").join("setup.json"));
    }
    let state = var("XDG_STATE_HOME").or_else(|| Some(var("HOME")?.join(".local").join("state")))?;
    Some(state.join("totk-recomp").join("setup.json"))
}

/// The install the setup last ran on: `dest` of `recent`, when that folder holds an INSTALL_MARKER. The setup records the folder when
/// the player presses Start, so it may be unfinished: `ready` needs the game's file and the marker's `complete` not false (true once a
/// setup run finished; a marker from Zonith 0.5.1 or older has no such entry, and there the game's file alone decides).
pub fn read_game(recent: &Path) -> Option<Game> {
    let json = |p: &Path| serde_json::from_str::<serde_json::Value>(&std::fs::read_to_string(p).ok()?).ok();
    let dir = PathBuf::from(json(recent)?.get("dest")?.as_str()?);
    if !dir.is_absolute() {
        return None;
    }
    let state = json(&dir.join(INSTALL_MARKER))?;
    let release = state.get("release").and_then(|v| v.as_str()).unwrap_or_default().to_string();
    let complete = state.get("complete").and_then(|v| v.as_bool()).unwrap_or(true);
    let ready = complete && dir.join(GAME_EXE).is_file();
    Some(Game { dir, release, ready })
}

/// The downloaded setup is newer than the release that built the game: the setup has to run on it once more ("v0.5.2" against "0.5.1").
/// A release that is no version (a development build's) is never stale.
pub fn game_stale(release: &str, setup_tag: &str) -> bool {
    crate::releases::is_newer(setup_tag, release)
}

/// A game Zonith's setup installed somewhere inside `dir` (its INSTALL_MARKER), without following links.
pub fn holds_install(dir: &Path) -> bool {
    let mut todo = vec![dir.to_path_buf()];
    while let Some(d) = todo.pop() {
        if d.join(INSTALL_MARKER).is_file() {
            return true;
        }
        for e in std::fs::read_dir(&d).map(|r| r.flatten().collect::<Vec<_>>()).unwrap_or_default() {
            if e.file_type().is_ok_and(|t| t.is_dir()) {
                todo.push(e.path());
            }
        }
    }
    false
}

/// Deletes the folders of older setups once the new one is recorded (`keep`: its folder). Only the launcher's own release folders
/// (`<tag>`, `<tag>.old`) go, and never one holding a game: the setup window starts in the launcher's folder (its working directory), so
/// its folder picker opens there, and a player may install the game into it. The setup app's starter exits at once, so the launcher
/// cannot tell whether an old setup window is still open: each folder is renamed first, which Windows refuses while a program has its
/// working directory in it (as the setup window does), and a folder that will not rename is left for the next update rather than half
/// deleted under the window.
pub fn remove_old(zonith: &Path, keep: &str) {
    for e in std::fs::read_dir(zonith).map(|r| r.flatten().collect::<Vec<_>>()).unwrap_or_default() {
        let name = e.file_name().to_string_lossy().into_owned();
        if !e.file_type().is_ok_and(|t| t.is_dir()) || name == keep {
            continue;
        }
        let tag = name.strip_suffix(".old").unwrap_or(&name);
        if crate::releases::Version::parse(tag).is_none() || holds_install(&e.path()) {
            continue;
        }
        let gone = if name.ends_with(".old") { e.path() } else { zonith.join(format!("{name}.old")) };
        if gone == e.path() || std::fs::rename(e.path(), &gone).is_ok() {
            let _ = std::fs::remove_dir_all(&gone);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const HEX: &str = "9a44d60023efd21255ba43b88ca72480d81330899ace099845df3e72da0917b2";

    fn list() -> Vec<GhRelease> {
        let asset = |name: &str, digest: Option<&str>| json!({"name": name, "size": 10, "browser_download_url": format!("https://x/{name}"), "digest": digest});
        let d = format!("sha256:{HEX}");
        let body = json!([
            {"tag_name": "v0.6.0-rc.1", "prerelease": true, "assets": [asset("TotK-Setup-0.6.0-windows.zip", Some(&d)), asset("TotK-Setup-x86_64.AppImage", Some(&d))]},
            {"tag_name": "v0.5.1", "name": "Zonith 0.5.1", "assets": [asset("TotK-Setup-0.5.1-windows.zip", None), asset("TotK-Setup-x86_64.AppImage", Some(&d))]},
            {"tag_name": "v0.5.0", "name": "", "published_at": "2026-11-19T03:33:51Z", "body": "notes", "html_url": "https://github.com/Dittobyte/Zonith/releases/tag/v0.5.0",
             "assets": [asset("TotK-Setup-0.5.0-windows.zip", Some(&d)), asset("TotK-Setup-x86_64.AppImage", Some(&d))]},
            {"tag_name": "v9.9.9", "draft": true, "assets": []}
        ]);
        parse_releases(body.to_string().as_bytes()).unwrap()
    }

    #[test]
    fn digests() {
        assert_eq!(parse_digest(&format!("sha256:{HEX}")).as_deref(), Some(HEX));
        assert_eq!(parse_digest(&format!("sha256:{}", HEX.to_uppercase())).as_deref(), Some(HEX));
        assert!(parse_digest("sha512:abcd").is_none());
        assert!(parse_digest("sha256:abc").is_none());
    }

    #[test]
    fn latest_skips_prereleases_drafts_and_unchecked_assets() {
        let l = list();
        assert_eq!(l.len(), 3, "the draft is dropped");
        let w = latest(&l, "-windows.zip").unwrap();
        assert_eq!((w.tag.as_str(), w.name.as_str(), w.asset.as_str(), w.sha256.as_str()), ("v0.5.0", "Zonith v0.5.0", "TotK-Setup-0.5.0-windows.zip", HEX));
        assert_eq!(latest(&l, ".AppImage").unwrap().tag, "v0.5.1");
        assert!(latest(&l, ".dmg").is_none());
        let sent = serde_json::to_value(&w).unwrap();
        assert!(sent.get("sha256").is_none() && sent.get("download_url").is_none());
        let r = rows(&l);
        assert_eq!(r.iter().map(|r| r.tag.as_str()).collect::<Vec<_>>(), ["v0.6.0-rc.1", "v0.5.1", "v0.5.0"]);
        assert!(r[0].prerelease && !r[1].prerelease);
    }

    #[test]
    fn versions_with_and_without_v() {
        assert!(update_available("0.5.0", "v0.5.1"));
        assert!(update_available("v0.5.0", "v0.6.0"));
        assert!(!update_available("v0.5.0", "v0.5.0"));
        assert!(!update_available("0.6.0", "v0.5.0"));
        assert!(!update_available("", "v0.5.0"));
    }

    #[test]
    fn current_needs_its_app() {
        let dir = std::env::temp_dir().join(format!("zonith-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        assert!(read_current(&dir).is_none());
        let asset = "TotK-Setup-x86_64.AppImage";
        std::fs::write(dir.join(CURRENT_FILE), json!({"tag": "v0.5.0", "asset": asset}).to_string()).unwrap();
        assert!(read_current(&dir).is_none(), "no setup app yet");
        let app = app_path(&dir, &Current { tag: "v0.5.0".into(), asset: asset.into() });
        std::fs::create_dir_all(app.parent().unwrap()).unwrap();
        std::fs::write(&app, b"x").unwrap();
        assert_eq!(read_current(&dir), Some(Current { tag: "v0.5.0".into(), asset: asset.into() }));
        std::fs::write(dir.join(CURRENT_FILE), "not json").unwrap();
        assert!(read_current(&dir).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_game_the_setup_built() {
        let dir = std::env::temp_dir().join(format!("zonith-game-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let game = dir.join("TotK");
        std::fs::create_dir_all(&game).unwrap();
        let recent = dir.join("setup.json");
        assert!(read_game(&recent).is_none(), "the setup never ran");
        std::fs::write(&recent, json!({"format": 1, "dest": game.to_str().unwrap()}).to_string()).unwrap();
        assert!(read_game(&recent).is_none(), "the folder holds no install");
        let marker = |v: serde_json::Value| std::fs::write(game.join(INSTALL_MARKER), v.to_string()).unwrap();
        marker(json!({"format": 1, "release": "0.5.1", "steps": {}}));
        assert_eq!(read_game(&recent), Some(Game { dir: game.clone(), release: "0.5.1".into(), ready: false }), "no game file yet");
        std::fs::write(game.join(GAME_EXE), b"x").unwrap();
        assert!(read_game(&recent).unwrap().ready, "0.5.1 and older: the game's file decides");
        marker(json!({"format": 1, "release": "0.5.2", "complete": false, "steps": {}}));
        assert!(!read_game(&recent).unwrap().ready, "the game's file is there, the first install did not finish");
        marker(json!({"format": 1, "release": "0.5.2", "complete": true, "steps": {}}));
        assert!(read_game(&recent).unwrap().ready);
        std::fs::write(&recent, json!({"dest": "TotK"}).to_string()).unwrap();
        assert!(read_game(&recent).is_none(), "a relative folder is nobody's");
        std::fs::write(&recent, "not json").unwrap();
        assert!(read_game(&recent).is_none());
        assert!(game_stale("0.5.1", "v0.5.2") && !game_stale("0.5.2", "v0.5.2") && !game_stale("0.5.3", "v0.5.2"));
        assert!(!game_stale("", "v0.5.2") && !game_stale("unknown", "v0.5.2"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn tags_and_progress() {
        assert_eq!(safe_tag("v0.5.0"), "v0.5.0");
        assert_eq!(safe_tag("../x"), "..x");
        assert_eq!(safe_tag(".."), "latest");
        assert_eq!(safe_tag("/"), "latest");
        assert_eq!(ZonithProgress::download(50, 100).pct, Some(45.0));
        assert_eq!(ZonithProgress::download(0, 0).pct, Some(0.0));
        assert_eq!(ZonithProgress::unpack().pct, Some(90.0));
        assert_eq!(ZonithProgress::end("done", None).pct, Some(100.0));
        assert_eq!(ZonithProgress::end("error", Some("x".into())).pct, None);
    }

    #[test]
    fn old_setups_go_files_stay() {
        let dir = std::env::temp_dir().join(format!("zonith-old-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for d in ["v0.5.0/TotK-Setup-windows", "v0.5.1", "v0.4.0.old"] {
            std::fs::create_dir_all(dir.join(d)).unwrap();
        }
        std::fs::write(dir.join("v0.5.0/TotK-Setup-windows/x"), b"x").unwrap();
        std::fs::write(dir.join(CURRENT_FILE), "{}").unwrap();
        std::fs::write(dir.join("TotK-Setup-x86_64.AppImage"), b"x").unwrap();
        // games the player installed into the launcher's folder: next to the setups, inside an old one, and a folder that is no release
        for d in ["v0.4.1/TotK-Setup-windows/TotK", "TotK", "v0.3.0.old/game", "Zonith games/empty"] {
            std::fs::create_dir_all(dir.join(d)).unwrap();
        }
        for d in ["v0.4.1/TotK-Setup-windows/TotK", "TotK", "v0.3.0.old/game"] {
            std::fs::write(dir.join(d).join(INSTALL_MARKER), b"{}").unwrap();
        }
        remove_old(&dir, "v0.5.1");
        let mut left: Vec<String> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        left.sort();
        assert_eq!(left, ["TotK", "TotK-Setup-x86_64.AppImage", "Zonith games", CURRENT_FILE, "v0.3.0.old", "v0.4.1", "v0.5.1"]);
        assert!(dir.join("v0.4.1/TotK-Setup-windows/TotK").join(INSTALL_MARKER).is_file());
        assert!(holds_install(&dir.join("v0.4.1")) && !holds_install(&dir.join("v0.5.1")));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
