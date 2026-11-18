//! Add to Steam: puts the launcher into Steam as a Non-Steam Game ("Recompia", or "Recompia (Testing)" on the Testing channel) with the
//! launch option `--play`, and copies the artwork into Steam's `grid` folder, so Game Mode and Big Picture show it without a search.
//!
//! Files (the app id rule and the file layout are the ones Steam ROM Manager uses):
//!  * `<Steam>/userdata/<account>/config/shortcuts.vdf`: Steam's binary key-value file, one numbered entry per shortcut. Steam keeps it in
//!    memory and writes it when it closes, so it is only edited while Steam is not running.
//!  * `<Steam>/userdata/<account>/config/grid/<id>.png` (wide), `<id>p.png` (portrait), `<id>_hero.png`, `<id>_logo.png`, `<id>_icon.png`,
//!    where `<id>` is the entry's app id. An entry we add gets the short app id: the CRC-32 of the quoted exe path followed by the
//!    name, with the top bit set. An entry made under the launcher's other file name (before the release rename) keeps the id it has
//!    when it is pointed at this file, so Steam keeps its play time, collections, controller layout and artwork with it.
//! The old file is copied to `shortcuts.vdf.recompia-backup` before the first change and that copy is kept (never overwritten, so it stays the
//! file the user had). Only our own entry and our own artwork are added,
//! replaced or removed; other shortcuts are kept as they are (unknown fields too).
use std::fs;
use std::path::{Path, PathBuf};

use crate::branding::{self, Branding};
use crate::fsutil::{self, AtomicFail};

use serde::Serialize;

use crate::release_names::{self, Host};
use crate::steam_vdf::{get_int, get_str, parse, set_str, shortcuts, shortcuts_mut, write, Val};

// ---- the app id ----
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { 0xedb8_8320 ^ (crc >> 1) } else { crc >> 1 };
        }
    }
    !crc
}

/// Steam's short app id of a Non-Steam shortcut: the CRC-32 of the exe text (as written in shortcuts.vdf, with its quotes) followed by
/// the name, top bit set. It names the artwork files, and is stored in the entry as the same 32 bits.
pub fn short_id(exe: &str, name: &str) -> u32 {
    crc32(format!("{exe}{name}").as_bytes()) | 0x8000_0000
}

// ---- what is added ----
#[derive(Clone, Debug)]
pub struct Spec {
    pub name: String,
    pub exe: String,        // quoted, as Steam writes it
    pub start_dir: String,  // quoted
    pub launch: String,
    /// the artwork, and the Testing channel's names and icon
    pub look: Branding,
    /// The launcher's other file name beside it (release_names::other_name), quoted: a shortcut made under that name is ours too, and
    /// Add to Steam points it at `exe`. None for a launcher the player renamed.
    pub other_exe: Option<String>,
}

fn entry_for(spec: &Spec, icon_path: &str) -> Vec<(String, Val)> {
    let s = |k: &str, v: &str| (k.to_string(), Val::Str(v.to_string()));
    let n = |k: &str, v: u32| (k.to_string(), Val::Int(v));
    vec![
        n("appid", short_id(&spec.exe, &spec.name)),
        s("AppName", &spec.name),
        s("Exe", &spec.exe),
        s("StartDir", &spec.start_dir),
        s("icon", icon_path),
        s("ShortcutPath", ""),
        s("LaunchOptions", &spec.launch),
        n("IsHidden", 0),
        n("AllowDesktopConfig", 1),
        n("AllowOverlay", 1),
        n("OpenVR", 0),
        n("Devkit", 0),
        s("DevkitGameID", ""),
        n("DevkitOverrideAppID", 0),
        n("LastPlayTime", 0),
        s("FlatpakAppID", ""),
        ("tags".to_string(), Val::Map(Vec::new())),
    ]
}

/// How an entry is ours: it starts this launcher file, or the same launcher under its other file name.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Ours {
    Here,
    OtherName,
}

fn ours(entry: &[(String, Val)], spec: &Spec) -> Option<Ours> {
    if get_str(entry, "AppName") != Some(spec.name.as_str()) {
        return None;
    }
    match get_str(entry, "Exe") {
        Some(exe) if exe == spec.exe => Some(Ours::Here),
        Some(exe) if spec.other_exe.as_deref() == Some(exe) => Some(Ours::OtherName),
        _ => None,
    }
}

fn has(root: &[(String, Val)], spec: &Spec, how: Ours) -> bool {
    shortcuts(root).iter().any(|(_, e)| matches!(e, Val::Map(e) if ours(e, spec) == Some(how)))
}

/// The ids our artwork is named by: the app id of each of our entries (the one that starts this file first), or, with none, the id a
/// new entry gets.
fn art_ids(root: &[(String, Val)], spec: &Spec) -> Vec<u32> {
    let mut ids = Vec::new();
    for how in [Ours::Here, Ours::OtherName] {
        for (_, e) in shortcuts(root) {
            match e {
                Val::Map(e) if ours(e, spec) == Some(how) => {
                    let id = get_int(e, "appid").unwrap_or_else(|| short_id(&spec.exe, &spec.name));
                    if !ids.contains(&id) {
                        ids.push(id);
                    }
                }
                _ => {}
            }
        }
    }
    if ids.is_empty() {
        ids.push(short_id(&spec.exe, &spec.name));
    }
    ids
}

/// Points an entry at `spec`'s launcher file and folder. Everything else in it stays: the app id, and what Steam or the player set.
fn point_at(entry: &mut Vec<(String, Val)>, spec: &Spec) {
    set_str(entry, "Exe", &spec.exe);
    set_str(entry, "StartDir", &spec.start_dir);
}

/// Adds our entry, or updates the one we added before in place (the one that starts this file, else one under the other file name).
/// Other entries are untouched.
pub fn add_entry(root: &mut Vec<(String, Val)>, spec: &Spec, icon_path: &str) {
    let list = shortcuts_mut(root);
    let find = |how| list.iter().position(|(_, v)| matches!(v, Val::Map(e) if ours(e, spec) == Some(how)));
    if let Some(at) = find(Ours::Here).or_else(|| find(Ours::OtherName)) {
        if let Val::Map(e) = &mut list[at].1 {
            point_at(e, spec);
            set_str(e, "icon", icon_path);
            set_str(e, "LaunchOptions", &spec.launch);
        }
        return;
    }
    let next = list.iter().filter_map(|(k, _)| k.parse::<u32>().ok()).max().map_or(0, |m| m + 1);
    list.push((next.to_string(), Val::Map(entry_for(spec, icon_path))));
}

/// Takes our entries out (under either file name); the numbers of the others are kept (Steam does not need them without gaps). True
/// when there was one.
pub fn remove_entry(root: &mut Vec<(String, Val)>, spec: &Spec) -> bool {
    let list = shortcuts_mut(root);
    let before = list.len();
    list.retain(|(_, v)| !matches!(v, Val::Map(e) if ours(e, spec).is_some()));
    list.len() != before
}

/// Our entry that starts this launcher file is there.
pub fn has_entry(root: &[(String, Val)], spec: &Spec) -> bool {
    has(root, spec, Ours::Here)
}

/// Points our entries under the other file name at this file, unless one already starts it. True when one changed.
pub fn repoint_entries(root: &mut Vec<(String, Val)>, spec: &Spec) -> bool {
    if has(root, spec, Ours::Here) {
        return false;
    }
    let mut changed = false;
    for (_, v) in shortcuts_mut(root).iter_mut() {
        match v {
            Val::Map(e) if ours(e, spec) == Some(Ours::OtherName) => {
                point_at(e, spec);
                changed = true;
            }
            _ => {}
        }
    }
    changed
}

// ---- Steam's folders ----
fn roots() -> Vec<PathBuf> {
    roots_in(std::env::var_os("HOME").map(PathBuf::from))
}

/// The Steam installs under `home`: Linux keeps them under `.local/share`, macOS in `Library/Application Support`, and the Flatpak
/// in `.var`. Split from `roots()` so a test can point it at a folder of its own.
fn roots_in(home: Option<PathBuf>) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    #[cfg(unix)]
    if let Some(home) = home {
        #[cfg(target_os = "macos")]
        v.push(home.join("Library/Application Support/Steam"));
        #[cfg(not(target_os = "macos"))]
        {
            v.push(home.join(".local/share/Steam"));
            v.push(home.join(".steam/steam"));
            v.push(home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"));
        }
    }
    #[cfg(not(unix))]
    let _ = home;
    #[cfg(windows)]
    {
        for var in ["ProgramFiles(x86)", "ProgramFiles"] {
            if let Some(p) = std::env::var_os(var) {
                v.push(PathBuf::from(p).join("Steam"));
            }
        }
        v.push(PathBuf::from(r"C:\Program Files (x86)\Steam"));
    }
    let mut seen: Vec<PathBuf> = Vec::new();
    let mut out = Vec::new();
    for p in v {
        if !p.join("userdata").is_dir() {
            continue;
        }
        let real = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
        if seen.contains(&real) {
            continue;
        }
        seen.push(real);
        out.push(p);
    }
    out
}

/// The account folders of a Steam install (the numeric ones, not the shared 0).
fn accounts(root: &Path) -> Vec<PathBuf> {
    let Ok(rd) = fs::read_dir(root.join("userdata")) else { return Vec::new() };
    let mut v: Vec<PathBuf> = rd
        .flatten()
        .filter(|e| e.path().is_dir())
        .filter(|e| {
            let n = e.file_name().to_string_lossy().into_owned();
            n != "0" && !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit())
        })
        .map(|e| e.path())
        .collect();
    v.sort();
    v
}

fn steam_running() -> bool {
    #[cfg(target_os = "linux")]
    {
        let Ok(rd) = fs::read_dir("/proc") else { return false };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.bytes().all(|b| b.is_ascii_digit()) {
                continue;
            }
            if let Ok(c) = fs::read_to_string(e.path().join("comm")) {
                if c.trim() == "steam" {
                    return true;
                }
            }
        }
        false
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        std::process::Command::new("tasklist")
            .args(["/FI", "IMAGENAME eq steam.exe", "/NH"])
            .creation_flags(0x0800_0000)
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_ascii_lowercase().contains("steam.exe"))
            .unwrap_or(false)
    }
    #[cfg(target_os = "macos")]
    {
        // Steam runs as steam_osx. Its own folder is checked too, because pgrep is not guaranteed on a Mac without the developer tools.
        if std::process::Command::new("pgrep").arg("-x").arg("steam_osx").output().map(|o| o.status.success()).unwrap_or(false) {
            return true;
        }
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join("Library/Application Support/Steam/logs/connection_log.txt"))
            .is_some_and(|log| fs::metadata(log).and_then(|m| m.modified()).map(|t| t.elapsed().map(|e| e.as_secs() < 120).unwrap_or(false)).unwrap_or(false))
    }
    #[cfg(not(any(target_os = "linux", windows, target_os = "macos")))]
    {
        false
    }
}

// ---- reading and writing one account ----
fn art_files(grid: &Path, id: u32) -> [(PathBuf, &'static str); 5] {
    [
        (grid.join(format!("{id}.png")), "wide"),
        (grid.join(format!("{id}p.png")), "portrait"),
        (grid.join(format!("{id}_hero.png")), "hero"),
        (grid.join(format!("{id}_logo.png")), "logo"),
        (grid.join(format!("{id}_icon.png")), "icon"),
    ]
}

fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    fsutil::write_atomic(path, data).map_err(|fail| match fail {
        AtomicFail::Write { error, .. } => format!("could not write {}: {error}", path.display()),
        AtomicFail::Replace { error } => format!("could not replace {}: {error}", path.display()),
    })
}

fn load_root(file: &Path) -> Result<Vec<(String, Val)>, String> {
    match fs::read(file) {
        Ok(b) => parse(&b).map_err(|e| format!("Steam's shortcut list could not be read ({e}). Nothing was changed.")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(format!("could not read {}: {e}", file.display())),
    }
}

/// Keeps a copy of Steam's shortcut list as the user had it: made once, the first time we change an existing file, and never overwritten
/// (a later add or remove would otherwise replace it with a file that already has our entries).
fn save_backup(cfg: &Path, file: &Path) -> Result<(), String> {
    let backup = cfg.join("shortcuts.vdf.recompia-backup");
    if file.exists() && !backup.exists() {
        fs::copy(file, &backup).map_err(|e| format!("could not save a backup of Steam's shortcut list: {e}"))?;
    }
    Ok(())
}

/// Adds the shortcut and the artwork for one account folder. The file is only rewritten after it parsed and the old one is saved.
pub fn add_to_account(account: &Path, spec: &Spec) -> Result<(), String> {
    let cfg = account.join("config");
    let grid = cfg.join("grid");
    fs::create_dir_all(&grid).map_err(|e| format!("could not create {}: {e}", grid.display()))?;
    let file = cfg.join("shortcuts.vdf");
    let mut root = load_root(&file)?;
    let id = art_ids(&root, spec)[0];
    for (path, kind) in art_files(&grid, id) {
        write_atomic(&path, branding::steam_art(kind, &spec.look))?;
    }
    let icon_path = grid.join(format!("{id}_icon.png")).to_string_lossy().into_owned();
    add_entry(&mut root, spec, &icon_path);
    save_backup(&cfg, &file)?;
    write_atomic(&file, &write(&root))
}

/// Takes our shortcut out and deletes our artwork files (only those that are still exactly ours). True when the shortcut was there.
pub fn remove_from_account(account: &Path, spec: &Spec) -> Result<bool, String> {
    let cfg = account.join("config");
    let file = cfg.join("shortcuts.vdf");
    let mut root = load_root(&file)?;
    let ids = art_ids(&root, spec);
    let had = remove_entry(&mut root, spec);
    if had {
        save_backup(&cfg, &file)?;
        write_atomic(&file, &write(&root))?;
    }
    for (path, kind) in ids.into_iter().flat_map(|id| art_files(&cfg.join("grid"), id)) {
        if let Ok(cur) = fs::read(&path) {
            if branding::is_our_steam_art(kind, spec.look.testing, &cur) {
                let _ = fs::remove_file(&path);
            }
        }
    }
    Ok(had)
}

// ---- what the launcher calls ----
#[derive(Serialize, Default, Clone, Debug)]
pub struct Status {
    pub found: bool,
    pub running: bool,
    pub accounts: usize,
    /// Accounts that have the shortcut that starts the game straight away (`--play`).
    pub game_added: usize,
    /// Accounts that have the shortcut that opens the launcher window.
    pub launcher_added: usize,
    /// Accounts whose game or launcher shortcut starts the launcher's other file name and none starts this one (made before the
    /// release rename; the small starter under the old name keeps it working). Add to Steam points it here and keeps its app id.
    pub game_other_name: usize,
    pub launcher_other_name: usize,
    pub game_name: String,
    pub launcher_name: String,
}

/// Which shortcut: the game itself (starts it at once, `--play`) or the launcher window.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Target {
    Game,
    Launcher,
}

impl Target {
    pub fn parse(s: &str) -> Result<Target, String> {
        match s {
            "game" => Ok(Target::Game),
            "launcher" => Ok(Target::Launcher),
            other => Err(format!("unknown Steam target \"{other}\"")),
        }
    }
}

/// The shortcut for this launcher's own file (the AppImage when run from one). The game shortcut is named "Recompia" (the name the game
/// goes by, so a SteamGridDB search for it finds the right title) and starts the game with `--play`; the launcher shortcut is "Recompia Launcher" and has no launch option.
pub fn spec_here(look: &Branding, target: Target) -> Result<Spec, String> {
    let exe = std::env::var_os("APPIMAGE")
        .map(PathBuf::from)
        .filter(|p| p.is_file())
        .or_else(|| std::env::current_exe().ok())
        .ok_or("could not find this launcher's own path")?;
    let dir = exe.parent().map(Path::to_path_buf).unwrap_or_default();
    let other_exe = release_names::other_name(&exe, Host::HERE).map(|p| format!("\"{}\"", p.display()));
    Ok(Spec {
        name: shortcut_name(look.testing, target),
        exe: format!("\"{}\"", exe.display()),
        start_dir: format!("\"{}\"", dir.display()),
        launch: if target == Target::Game { "--play".into() } else { String::new() },
        look: *look,
        other_exe,
    })
}

pub fn shortcut_name(testing: bool, target: Target) -> String {
    match (target, testing) {
        (Target::Game, false) => "Recompia".into(),
        (Target::Game, true) => "Recompia (Testing)".into(),
        (Target::Launcher, false) => "Recompia Launcher".into(),
        (Target::Launcher, true) => "Recompia Launcher (Testing)".into(),
    }
}

fn all_accounts() -> Vec<PathBuf> {
    roots().iter().flat_map(|r| accounts(r)).collect()
}

/// Accounts with our entry that starts this file, and accounts with only one under the other file name.
fn count_added(accs: &[PathBuf], spec: &Result<Spec, String>) -> (usize, usize) {
    let Ok(s) = spec else { return (0, 0) };
    let (mut here, mut other) = (0, 0);
    for root in accs.iter().filter_map(|a| load_root(&a.join("config").join("shortcuts.vdf")).ok()) {
        if has_entry(&root, s) {
            here += 1;
        } else if has(&root, s, Ours::OtherName) {
            other += 1;
        }
    }
    (here, other)
}

pub fn status(look: &Branding) -> Status {
    let testing = look.testing;
    let accs = all_accounts();
    let (game_added, game_other_name) = count_added(&accs, &spec_here(look, Target::Game));
    let (launcher_added, launcher_other_name) = count_added(&accs, &spec_here(look, Target::Launcher));
    Status {
        found: !roots().is_empty() && !accs.is_empty(),
        running: steam_running(),
        accounts: accs.len(),
        game_added,
        launcher_added,
        game_other_name,
        launcher_other_name,
        game_name: shortcut_name(testing, Target::Game),
        launcher_name: shortcut_name(testing, Target::Launcher),
    }
}

pub fn add(look: &Branding, target: Target) -> Result<String, String> {
    if steam_running() {
        return Err("Close Steam first (Steam, Exit), then press the button again. Steam only saves its shortcut list when it closes.".into());
    }
    let accs = all_accounts();
    if accs.is_empty() {
        return Err("Steam was not found on this PC, or nobody has signed in to it yet. Start Steam once, sign in, close it, and try again.".into());
    }
    let spec = spec_here(look, target)?;
    for a in &accs {
        add_to_account(a, &spec)?;
    }
    Ok(format!("Added \"{}\" to Steam for {} account{}. Start Steam again to see it, with its artwork.", spec.name, accs.len(), if accs.len() == 1 { "" } else { "s" }))
}

pub fn remove(look: &Branding, target: Target) -> Result<String, String> {
    if steam_running() {
        return Err("Close Steam first (Steam, Exit), then press the button again.".into());
    }
    let spec = spec_here(look, target)?;
    let mut n = 0;
    for a in all_accounts() {
        if remove_from_account(&a, &spec)? {
            n += 1;
        }
    }
    Ok(if n == 0 { "It was not in Steam.".into() } else { format!("Removed \"{}\" from Steam. Start Steam again.", spec.name) })
}

/// After a channel change: the shortcuts already in Steam get the channel's artwork. Only pictures that are still ours are replaced (a
/// player's own art from SteamGridDB stays), and shortcuts.vdf is not touched, so Steam may run; it shows the new pictures after a restart.
pub fn refresh_art(look: &Branding) -> Result<(), String> {
    for target in [Target::Game, Target::Launcher] {
        let spec = spec_here(look, target)?;
        for account in all_accounts() {
            let cfg = account.join("config");
            // read only: Steam may run
            let root = load_root(&cfg.join("shortcuts.vdf")).unwrap_or_default();
            let grid = cfg.join("grid");
            for (path, kind) in art_ids(&root, &spec).into_iter().flat_map(|id| art_files(&grid, id)) {
                let want = branding::steam_art(kind, look);
                match fs::read(&path) {
                    Ok(cur) if cur != want && branding::is_our_steam_art(kind, look.testing, &cur) => write_atomic(&path, want)?,
                    _ => {}
                }
            }
        }
    }
    Ok(())
}

/// What `repoint_here` did.
#[derive(Debug, PartialEq)]
pub enum Repointed {
    /// Steam runs, so nothing was changed (it writes its own copy of the list back when it closes); Add to Steam does it later.
    SteamRunning,
    /// The number of accounts whose list changed.
    Accounts(usize),
}

/// After the launcher's file was renamed (rename_migrate.rs): our shortcuts, of both channels, that start the launcher's other file
/// name in this folder start this file. Only Exe and StartDir change, so each keeps its app id and with it Steam's play time,
/// collections, controller layout and the artwork named by it.
pub fn repoint_here() -> Result<Repointed, String> {
    if steam_running() {
        return Ok(Repointed::SteamRunning);
    }
    let mut specs = Vec::new();
    for testing in [false, true] {
        for target in [Target::Game, Target::Launcher] {
            specs.push(spec_here(&Branding { testing }, target)?);
        }
    }
    let mut n = 0;
    for account in all_accounts() {
        if repoint_account(&account, &specs)? {
            n += 1;
        }
    }
    Ok(Repointed::Accounts(n))
}

/// `repoint_here` for one account folder. True when its list changed; it is only rewritten then, after the old one is saved.
fn repoint_account(account: &Path, specs: &[Spec]) -> Result<bool, String> {
    let cfg = account.join("config");
    let file = cfg.join("shortcuts.vdf");
    let mut root = load_root(&file)?;
    let mut changed = false;
    for spec in specs {
        changed |= repoint_entries(&mut root, spec);
    }
    if changed {
        save_backup(&cfg, &file)?;
        write_atomic(&file, &write(&root))?;
    }
    Ok(changed)
}

#[cfg(test)]
mod tests {
    use super::*;

    // unix only: Windows finds Steam under Program Files, not under a home folder a test can point at
    #[cfg(unix)]
    #[test]
    fn roots_find_the_platform_install() {
        let base = std::env::temp_dir().join(format!("steam_roots_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        #[cfg(target_os = "macos")]
        let steam = base.join("Library/Application Support/Steam");
        #[cfg(not(target_os = "macos"))]
        let steam = base.join(".local/share/Steam");
        fs::create_dir_all(steam.join("userdata").join("123").join("config")).unwrap();
        let roots = roots_in(Some(base.clone()));
        assert_eq!(roots, vec![steam.clone()]);
        assert_eq!(accounts(&steam).len(), 1);
        let _ = fs::remove_dir_all(&base);
    }

    const LOOK: Branding = Branding { testing: false };

    fn spec(testing: bool) -> Spec {
        Spec {
            name: if testing { "Recompia (Testing)" } else { "Recompia" }.into(),
            exe: "\"/home/deck/Games/Recompia/Recompia.AppImage\"".into(),
            start_dir: "\"/home/deck/Games/Recompia\"".into(),
            launch: "--play".into(),
            look: Branding { testing },
            other_exe: Some("\"/home/deck/Games/Recompia/PokopiaLauncher.AppImage\"".into()),
        }
    }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("recompia-steam-test-{}-{tag}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn crc32_known_values() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
        assert_eq!(crc32(b""), 0);
        // the rule: CRC-32 of exe + name, top bit set
        assert_eq!(short_id("a", "b") & 0x8000_0000, 0x8000_0000);
        assert_eq!(short_id("a", "b"), crc32(b"ab") | 0x8000_0000);
    }

    #[test]
    fn vdf_round_trip_keeps_everything() {
        let mut root = Vec::new();
        add_entry(&mut root, &spec(false), "/x/icon.png");
        let other = Spec { name: "Other Game".into(), exe: "\"/bin/other\"".into(), start_dir: "\"/bin\"".into(), launch: "-x".into(), look: LOOK, other_exe: None };
        add_entry(&mut root, &other, "");
        let bytes = write(&root);
        assert_eq!(bytes[0], 0);
        assert_eq!(*bytes.last().unwrap(), 8);
        let back = parse(&bytes).unwrap();
        assert_eq!(back, root);
        assert_eq!(write(&back), bytes);
    }

    #[test]
    fn add_replaces_ours_and_keeps_others() {
        let mut root = Vec::new();
        let other = Spec { name: "Other".into(), exe: "\"/bin/o\"".into(), start_dir: "\"/bin\"".into(), launch: "".into(), look: LOOK, other_exe: None };
        add_entry(&mut root, &other, "");
        add_entry(&mut root, &spec(false), "a");
        add_entry(&mut root, &spec(false), "b");   // the second add updates the entry, it does not make a second one
        let list = match &root[0].1 { Val::Map(m) => m.clone(), _ => panic!() };
        assert_eq!(list.len(), 2);
        assert!(has_entry(&root, &spec(false)));
        assert!(!has_entry(&root, &spec(true)));   // the Testing shortcut is a different one
        assert!(remove_entry(&mut root, &spec(false)));
        assert!(!remove_entry(&mut root, &spec(false)));
        assert!(has_entry(&root, &other));
    }

    #[test]
    fn game_and_launcher_are_two_shortcuts() {
        let g = shortcut_name(false, Target::Game);
        let l = shortcut_name(false, Target::Launcher);
        assert_eq!(g, "Recompia");
        assert_eq!(l, "Recompia Launcher");
        assert_eq!(shortcut_name(true, Target::Game), "Recompia (Testing)");
        assert_eq!(shortcut_name(true, Target::Launcher), "Recompia Launcher (Testing)");
        assert!(Target::parse("game").is_ok() && Target::parse("launcher").is_ok() && Target::parse("x").is_err());
        let acc = tmp("two");
        let exe = "\"/h/PokopiaLauncher.AppImage\"".to_string();
        let game = Spec { name: g, exe: exe.clone(), start_dir: "\"/h\"".into(), launch: "--play".into(), look: LOOK, other_exe: None };
        let launcher = Spec { name: l, exe, start_dir: "\"/h\"".into(), launch: String::new(), look: LOOK, other_exe: None };
        assert_ne!(short_id(&game.exe, &game.name), short_id(&launcher.exe, &launcher.name));
        add_to_account(&acc, &game).unwrap();
        add_to_account(&acc, &launcher).unwrap();
        let root = parse(&fs::read(acc.join("config/shortcuts.vdf")).unwrap()).unwrap();
        assert!(has_entry(&root, &game) && has_entry(&root, &launcher));
        // removing one leaves the other
        assert!(remove_from_account(&acc, &game).unwrap());
        let root = parse(&fs::read(acc.join("config/shortcuts.vdf")).unwrap()).unwrap();
        assert!(!has_entry(&root, &game) && has_entry(&root, &launcher));
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn the_backup_stays_the_file_the_user_had() {
        let acc = tmp("backup");
        let cfg = acc.join("config");
        fs::create_dir_all(&cfg).unwrap();
        let mut b = Vec::new();
        b.extend_from_slice(&[0]); b.extend_from_slice(b"shortcuts\0");
        b.extend_from_slice(&[0]); b.extend_from_slice(b"0\0");
        b.push(1); b.extend_from_slice(b"AppName\0Mine\0");
        b.extend_from_slice(&[8, 8, 8]);
        fs::write(cfg.join("shortcuts.vdf"), &b).unwrap();
        let mut launcher = spec(false);
        launcher.name = "Recompia Launcher".into();
        launcher.launch = String::new();
        add_to_account(&acc, &spec(false)).unwrap();
        add_to_account(&acc, &launcher).unwrap();
        assert!(remove_from_account(&acc, &spec(false)).unwrap());
        assert_eq!(fs::read(cfg.join("shortcuts.vdf.recompia-backup")).unwrap(), b);
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn unknown_fields_and_case_survive() {
        // an entry written by Steam with lower-case names and an extra field
        let mut b = Vec::new();
        b.extend_from_slice(&[0]); b.extend_from_slice(b"shortcuts\0");
        b.extend_from_slice(&[0]); b.extend_from_slice(b"0\0");
        b.push(1); b.extend_from_slice(b"appname\0Mine\0");
        b.push(1); b.extend_from_slice(b"exe\0\"/m\"\0");
        b.push(2); b.extend_from_slice(b"Extra\0"); b.extend_from_slice(&7u32.to_le_bytes());
        b.extend_from_slice(&[8, 8, 8]);
        let mut root = parse(&b).unwrap();
        add_entry(&mut root, &spec(false), "");
        let out = write(&root);
        let back = parse(&out).unwrap();
        let s = String::from_utf8_lossy(&out).into_owned();
        assert!(s.contains("appname") && s.contains("Extra") && s.contains("Recompia"));
        assert_eq!(back, root);
    }

    #[test]
    fn account_add_and_remove_end_to_end() {
        let acc = tmp("e2e");
        let s = spec(false);
        add_to_account(&acc, &s).unwrap();
        let cfg = acc.join("config");
        let id = short_id(&s.exe, &s.name);
        for n in [format!("{id}.png"), format!("{id}p.png"), format!("{id}_hero.png"), format!("{id}_logo.png"), format!("{id}_icon.png")] {
            assert!(cfg.join("grid").join(&n).is_file(), "{n} missing");
        }
        let root = parse(&fs::read(cfg.join("shortcuts.vdf")).unwrap()).unwrap();
        assert!(has_entry(&root, &s));
        // the second add changes nothing but keeps a backup of the first file
        let first = fs::read(cfg.join("shortcuts.vdf")).unwrap();
        add_to_account(&acc, &s).unwrap();
        assert_eq!(fs::read(cfg.join("shortcuts.vdf")).unwrap(), first);
        assert_eq!(fs::read(cfg.join("shortcuts.vdf.recompia-backup")).unwrap(), first);
        // a file the user changed is not deleted on remove
        fs::write(cfg.join("grid").join(format!("{id}_hero.png")), b"mine").unwrap();
        assert!(remove_from_account(&acc, &s).unwrap());
        assert!(!cfg.join("grid").join(format!("{id}.png")).exists());
        assert!(cfg.join("grid").join(format!("{id}_hero.png")).exists());
        assert!(!has_entry(&parse(&fs::read(cfg.join("shortcuts.vdf")).unwrap()).unwrap(), &s));
        assert!(!remove_from_account(&acc, &s).unwrap());
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn unreadable_file_is_left_alone() {
        let acc = tmp("bad");
        let cfg = acc.join("config");
        fs::create_dir_all(&cfg).unwrap();
        fs::write(cfg.join("shortcuts.vdf"), [9u8, 1, 2, 3]).unwrap();
        let err = add_to_account(&acc, &spec(false)).unwrap_err();
        assert!(err.contains("Nothing was changed"), "{err}");
        assert_eq!(fs::read(cfg.join("shortcuts.vdf")).unwrap(), vec![9u8, 1, 2, 3]);
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn accounts_skip_the_shared_folder() {
        let root = tmp("acc");
        for n in ["0", "93249718", "notanumber"] {
            fs::create_dir_all(root.join("userdata").join(n)).unwrap();
        }
        let a = accounts(&root);
        assert_eq!(a.len(), 1);
        assert!(a[0].ends_with("93249718"));
        let _ = fs::remove_dir_all(&root);
    }

    /// What a launcher before the release rename wrote: our entry starting the old file name, with the id of that name, plus what
    /// Steam and the player added to it since.
    fn old_name_account(tag: &str) -> (PathBuf, u32) {
        let acc = tmp(tag);
        let mut old = spec(false);
        old.exe = old.other_exe.take().unwrap();
        add_to_account(&acc, &old).unwrap();
        let file = acc.join("config/shortcuts.vdf");
        let mut root = parse(&fs::read(&file).unwrap()).unwrap();
        for (_, e) in shortcuts_mut(&mut root).iter_mut() {
            if let Val::Map(e) = e {
                e.retain(|(k, _)| k != "LastPlayTime");
                e.push(("LastPlayTime".into(), Val::Int(1_700_000_000)));
            }
        }
        fs::write(&file, write(&root)).unwrap();
        (acc, short_id(&old.exe, &old.name))
    }

    fn the_entry(acc: &Path) -> Vec<(String, Val)> {
        let root = parse(&fs::read(acc.join("config/shortcuts.vdf")).unwrap()).unwrap();
        let list = shortcuts(&root);
        assert_eq!(list.len(), 1, "one entry, not a second one");
        match &list[0].1 {
            Val::Map(e) => e.clone(),
            _ => panic!(),
        }
    }

    #[test]
    fn add_points_an_old_name_entry_here_and_keeps_its_id_play_time_and_art() {
        let (acc, old_id) = old_name_account("readd");
        let s = spec(false);
        assert_ne!(old_id, short_id(&s.exe, &s.name));
        let root = parse(&fs::read(acc.join("config/shortcuts.vdf")).unwrap()).unwrap();
        assert!(!has_entry(&root, &s) && has(&root, &s, Ours::OtherName), "the old entry counts as the other name, not as added");
        add_to_account(&acc, &s).unwrap();
        let e = the_entry(&acc);
        assert_eq!(get_str(&e, "Exe"), Some(s.exe.as_str()));
        assert_eq!(get_str(&e, "StartDir"), Some(s.start_dir.as_str()));
        assert_eq!(get_int(&e, "appid"), Some(old_id));
        assert_eq!(get_int(&e, "LastPlayTime"), Some(1_700_000_000));
        let grid = acc.join("config").join("grid");
        assert!(grid.join(format!("{old_id}_hero.png")).is_file(), "the art stays under the entry's id");
        assert!(!grid.join(format!("{}_hero.png", short_id(&s.exe, &s.name))).exists(), "no art under a new id");
        assert_eq!(get_str(&e, "icon"), Some(grid.join(format!("{old_id}_icon.png")).to_string_lossy().as_ref()));
        // removing takes the entry and its art, by the entry's id
        assert!(remove_from_account(&acc, &s).unwrap());
        assert!(!grid.join(format!("{old_id}.png")).exists());
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn repoint_changes_only_the_file_and_folder_and_runs_once() {
        let (acc, old_id) = old_name_account("repoint");
        let before = the_entry(&acc);
        let specs = [spec(false), spec(true)];
        assert!(repoint_account(&acc, &specs).unwrap());
        let after = the_entry(&acc);
        let s = spec(false);
        assert_eq!(get_str(&after, "Exe"), Some(s.exe.as_str()));
        assert_eq!(get_str(&after, "StartDir"), Some(s.start_dir.as_str()));
        assert_eq!(get_int(&after, "appid"), Some(old_id));
        let others = |e: &[(String, Val)]| e.iter().filter(|(k, _)| k != "Exe" && k != "StartDir").cloned().collect::<Vec<_>>();
        assert_eq!(others(&after), others(&before), "nothing else in the entry changed");
        assert!(!repoint_account(&acc, &specs).unwrap(), "the second run finds nothing to do and writes nothing");
        assert!(acc.join("config/shortcuts.vdf.recompia-backup").is_file());
        let _ = fs::remove_dir_all(&acc);
    }

    #[test]
    fn a_renamed_launcher_has_no_other_name_and_other_games_are_never_ours() {
        let mut root = Vec::new();
        let mut mine = spec(false);
        let stranger = Spec { exe: mine.other_exe.clone().unwrap(), name: "Another game".into(), ..spec(false) };
        add_entry(&mut root, &stranger, "");
        assert!(!repoint_entries(&mut root, &mine), "another game starting the same file is not ours");
        mine.other_exe = None;
        let mut old = spec(false);
        old.exe = old.other_exe.take().unwrap();
        add_entry(&mut root, &old, "");
        assert!(!repoint_entries(&mut root, &mine), "a launcher the player renamed knows no other name");
        assert_eq!(shortcuts(&root).len(), 2);
    }
}
