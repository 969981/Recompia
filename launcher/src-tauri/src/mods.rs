// SPDX-License-Identifier: GPL-3.0-or-later
//! The player's Modtopia mods in the game folder: the list the Mods page shows, its changes, and the hook plan Set up builds from.
//!
//! The Modtopia core owns the folders and files (contract 2: `<game>/mods/` with `modtopia.toml` and `.modtopia/`, `<game>/config/<id>.toml`,
//! `<game>/modtopia/mappings/`); this module reaches them only through it (modtopia_ffi.rs), never by editing them itself. Each call opens
//! a fresh scan, so a mod dropped into the folder by hand shows at the next look.
//!
//! Code the player has not allowed never runs: a code mod whose file is new or changed since the last yes (the core's MT_MOD_NEW_CODE) is
//! switched off at every scan, and switching it on again is the moment the page asks (mods_cmds.rs `mods_set_enabled` with `allow`).
//! After every change the plan is written again (`refresh_plan`): `<game>/mods/.modtopia/hooks.tsv`, or no file when no mod needs a hook
//! point. Set up builds those points in; `pkp_build.py --check` compares the plan's hash, and readiness.rs says when Set up must run again.

use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};

use crate::gamebuild;
use crate::modtopia_ffi::{self as ffi, Core, ModInfo, SettingInfo};

include!(concat!(env!("OUT_DIR"), "/modtopia_mappings.rs"));

pub fn mods_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("mods")
}

fn mappings_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("modtopia").join("mappings")
}

/// Writes the embedded mappings into `<game>/modtopia/mappings/`, each file only when its content differs (the runtime does the same).
fn write_mappings(game_dir: &Path) -> Result<(), String> {
    let dir = mappings_dir(game_dir);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    for (name, bytes) in MAPPINGS {
        let path = dir.join(name);
        if fs::read(&path).ok().as_deref() != Some(*bytes) {
            crate::fsutil::write_atomic(&path, bytes).map_err(|_| format!("Cannot write {}.", path.display()))?;
        }
    }
    Ok(())
}

/// The host name mods see: "Recompia <version>".
fn host_name() -> String {
    format!("Recompia {}", env!("CARGO_PKG_VERSION"))
}

/// A scan of the game's mods, with code the player has not allowed switched off first (that needs a second scan: the plan and the load
/// order are made by the scan).
fn open(game_dir: &Path) -> Core {
    if let Err(e) = write_mappings(game_dir) {
        eprintln!("mods: {e}");
    }
    let (mods, maps, host) = (mods_dir(game_dir), mappings_dir(game_dir), host_name());
    let core = Core::open(&mods, &maps, &host);
    let unallowed: Vec<String> = core.mods().into_iter().filter(|m| m.enabled && needs_yes(m)).map(|m| m.id).collect();
    if unallowed.is_empty() {
        return core;
    }
    for id in &unallowed {
        if core.set_enabled(id, false).is_err() {
            eprintln!("mods: could not switch off {id}, whose code is not allowed yet");
        }
    }
    drop(core);
    Core::open(&mods, &maps, &host)
}

/// A code mod whose file the player has not said yes to (new, or changed since).
fn needs_yes(m: &ModInfo) -> bool {
    m.flags & ffi::FLAG_CODE != 0 && m.flags & ffi::FLAG_NEW_CODE != 0
}

/// Writes the hook plan of the mods that are on to `<game>/mods/.modtopia/hooks.tsv`, only when it changed (its time is part of Set up's
/// check), or removes it when no mod needs a point. Returns the plan's hash ("" for none).
fn write_plan(core: &Core, game_dir: &Path) -> Result<String, String> {
    let path = game_dir.join(gamebuild::MOD_PLAN);
    if core.plan_count() == 0 {
        match fs::remove_file(&path) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(format!("Cannot remove {}: {}", path.display(), e)),
        }
        return Ok(String::new());
    }
    let dir = path.parent().unwrap();
    fs::create_dir_all(dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let tmp = crate::fsutil::temp_beside(&path);
    let written = core.plan_write(&tmp).map_err(|_| format!("Cannot write {}.", tmp.display()));
    let result = written.and_then(|()| {
        let new = fs::read(&tmp).map_err(|e| e.to_string())?;
        if fs::read(&path).ok() == Some(new) {
            return Ok(());
        }
        fs::rename(&tmp, &path).map_err(|e| format!("Cannot write {}: {}", path.display(), e))
    });
    let _ = fs::remove_file(&tmp);
    result.map(|()| core.plan_hash())
}

/// Scans the mods and writes their hook plan: before Play, before Set up and after every change on the Mods page.
pub fn refresh_plan(game_dir: &Path) -> Result<(), String> {
    write_plan(&open(game_dir), game_dir).map(|_| ())
}

/// One setting row of a mod.
#[derive(Serialize, Clone, Debug)]
pub struct SettingView {
    pub key: String,
    pub label: String,
    pub description: String,
    /// "bool", "number", "choice", "text" or "key"
    pub kind: &'static str,
    /// a change takes effect at the next start of the game
    pub restart: bool,
    pub min: f64,
    pub max: f64,
    pub step: f64,
    pub choices: Vec<String>,
    /// the value as the core shows it (On / Off, a number, the choice, the text)
    pub text: String,
    /// bool 0/1, the number, or the choice's index
    pub number: f64,
}

fn setting_view(s: SettingInfo) -> SettingView {
    let kind = match s.kind {
        ffi::SET_BOOL => "bool",
        ffi::SET_NUMBER => "number",
        ffi::SET_CHOICE => "choice",
        ffi::SET_KEY => "key",
        ffi::SET_TEXT => "text",
        _ => "text",
    };
    SettingView { key: s.key, label: s.label, description: s.description, kind, restart: s.restart, min: s.min, max: s.max, step: s.step, choices: s.choices, text: s.text, number: s.number }
}

/// One installed mod, as the Mods page shows it.
#[derive(Serialize, Clone, Debug)]
pub struct ModView {
    pub id: String,
    pub name: String,
    pub version: String,
    pub authors: String,
    pub license: String,
    pub description: String,
    /// its file or folder in mods/
    pub source: String,
    /// changes gameplay (everyone in a Link Play session needs it) rather than looks only
    pub gameplay: bool,
    /// a LayeredFS folder or zip without a mod.toml: the core guessed `gameplay` from its files
    pub guessed: bool,
    pub code: bool,
    /// a code mod the player has not said yes to (new or changed): switching it on asks first
    pub needs_yes: bool,
    /// the hosts it may reach, "" when none
    pub network: String,
    /// the player's switch
    pub enabled: bool,
    /// "ok" (loads), "off" (not this time), "error" (broken)
    pub state: &'static str,
    /// why it is off or broken, or a note; "" when there is nothing to say
    pub message: String,
    pub hook_points: i32,
    pub settings: Vec<SettingView>,
}

/// The Mods page's list.
#[derive(Serialize, Clone, Debug)]
pub struct ModsView {
    /// the mods folder, for the page's "Open mods folder"
    pub dir: String,
    /// installed mods: those that load, in load order, then the rest
    pub mods: Vec<ModView>,
    /// "id@version" of every gameplay mod that is on (what the other Link Play players need too)
    pub gameplay: Vec<String>,
    /// hook points the mods that are on need
    pub points: i32,
}

fn view_of(core: &Core, game_dir: &Path) -> ModsView {
    let mods = core
        .mods()
        .into_iter()
        .enumerate()
        .map(|(i, m)| ModView {
            gameplay: m.flags & ffi::FLAG_GAMEPLAY != 0,
            guessed: m.flags & ffi::FLAG_LAYEREDFS != 0 && m.flags & ffi::FLAG_KIND_GUESSED != 0,
            code: m.flags & ffi::FLAG_CODE != 0,
            needs_yes: needs_yes(&m),
            network: if m.flags & ffi::FLAG_NETWORK != 0 { m.network.clone() } else { String::new() },
            state: match m.state {
                ffi::STATE_OK => "ok",
                ffi::STATE_ERROR => "error",
                _ => "off",
            },
            settings: core.settings(i as i32).into_iter().map(setting_view).collect(),
            id: m.id,
            name: m.name,
            version: m.version,
            authors: m.authors,
            license: m.license,
            description: m.description,
            source: m.source,
            enabled: m.enabled,
            message: m.message,
            hook_points: m.hook_points,
        })
        .collect();
    ModsView { dir: crate::dump::fwd(&mods_dir(game_dir)), mods, gameplay: core.gameplay_mods(), points: core.plan_count() }
}

/// The installed mods, after writing their plan (a mod dropped in by hand changes it as much as a click does).
pub fn list(game_dir: &Path) -> Result<ModsView, String> {
    let core = open(game_dir);
    write_plan(&core, game_dir)?;
    Ok(view_of(&core, game_dir))
}

/// Opens a scan, applies `change` to it, then lists again with a fresh scan (the switches, the order and the plan are the scan's).
fn change(game_dir: &Path, change: impl FnOnce(&Core) -> Result<(), String>) -> Result<ModsView, String> {
    {
        let core = open(game_dir);
        change(&core)?;
    }
    list(game_dir)
}

fn known(core: &Core, id: &str) -> Result<i32, String> {
    core.find(id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))
}

const SAVE_FAILED: &str = "Could not save the mods' list in the mods folder.";

/// The player's switch. A code mod that is not allowed yet is switched on only with `allow` (the player said yes to the question).
pub fn set_enabled(game_dir: &Path, id: &str, on: bool, allow: bool) -> Result<ModsView, String> {
    change(game_dir, |core| {
        known(core, id)?;
        let m = core.mods().into_iter().find(|m| m.id == id).unwrap();
        if on && needs_yes(&m) {
            if !allow {
                return Err("This mod's code is not allowed yet.".into());
            }
            core.trust_mod(id).map_err(|_| format!("Could not record your yes for {}.", m.name))?;
        }
        core.set_enabled(id, on).map_err(|_| SAVE_FAILED.to_string())
    })
}

/// Where a mod is in the list the page shows: (its index, the ids of the list in order).
fn place(core: &Core, id: &str) -> Option<(usize, Vec<String>)> {
    let ids: Vec<String> = core.mods().into_iter().map(|m| m.id).collect();
    ids.iter().position(|m| m == id).map(|i| (i, ids))
}

/// One place up (-1) or down (+1) in the list as the page shows it. The core's order also holds mods that are off or broken, which the
/// list shows after the rest, so a step there may not move the mod past a visible neighbour: it steps on until it does, or reaches an
/// end of the order (then nothing more moves; a dependency can also hold a mod in place).
pub fn move_by(game_dir: &Path, id: &str, delta: i32) -> Result<ModsView, String> {
    let (start, ids) = place(&open(game_dir), id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))?;
    for _ in 0..ids.len() {
        let core = open(game_dir);
        match core.move_by(id, delta.signum()) {
            Ok(()) => {}
            Err(-4) => break, // MT_ERR_STATE: at an end of the order
            Err(_) => return Err(SAVE_FAILED.into()),
        }
        drop(core);
        if place(&open(game_dir), id).is_some_and(|(now, _)| now != start) {
            break;
        }
    }
    list(game_dir)
}

/// A new value for one of a mod's settings: a number (bools 0/1, choices by index) or a text, as the setting's kind takes it.
pub fn set_setting(game_dir: &Path, id: &str, key: &str, number: Option<f64>, text: Option<&str>) -> Result<ModsView, String> {
    change(game_dir, |core| {
        let m = known(core, id)?;
        let i = core.settings(m).iter().position(|s| s.key == key).ok_or_else(|| format!("{} has no setting {}.", id, key))? as i32;
        let r = match (number, text) {
            (Some(v), _) => core.set_number(m, i, v),
            (None, Some(t)) => core.set_text(m, i, t),
            (None, None) => return Err(format!("No value for {}.", key)),
        };
        r.map_err(|_| format!("Could not change {} of {}.", key, id))
    })
}

/// A mod's file or folder name as found in mods/: one plain name, never a path that leaves the folder.
fn plain_name(source: &str) -> bool {
    !source.is_empty() && source != "." && source != ".." && !source.starts_with('.') && !source.contains(['/', '\\', ':', '\0'])
}

/// Deletes a mod's file or folder from mods/ (the page asks first, like Settings > Profiles' Remove). Its settings in config/ stay, so
/// installing it again keeps them; the core clears its unpacked copy at the next scan.
pub fn remove(game_dir: &Path, id: &str) -> Result<ModsView, String> {
    let source = {
        let core = open(game_dir);
        known(&core, id)?;
        core.mods().into_iter().find(|m| m.id == id).unwrap().source
    };
    delete_source(game_dir, &source)?;
    list(game_dir)
}

/// Deletes one entry of mods/ by its name.
pub fn delete_source(game_dir: &Path, source: &str) -> Result<(), String> {
    if !plain_name(source) {
        return Err(format!("{} is not in the mods folder.", source));
    }
    let path = mods_dir(game_dir).join(source);
    let r = if path.is_dir() { fs::remove_dir_all(&path) } else { fs::remove_file(&path) };
    r.map_err(|e| format!("Could not delete {}: {}", path.display(), e))
}

/// The installed version of a mod and the name of its file in mods/, if it is there.
pub fn installed(game_dir: &Path, id: &str) -> Option<(String, String)> {
    let core = open(game_dir);
    core.mods().into_iter().find(|m| m.id == id).map(|m| (m.version, m.source))
}

/// Every installed mod's id and version (the catalog marks what is installed and what has a newer version).
pub fn installed_versions(game_dir: &Path) -> Vec<(String, String)> {
    open(game_dir).mods().into_iter().map(|m| (m.id, m.version)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_plain_names_are_deleted() {
        for ok in ["camera.pkmod", "my mod", "a-b_c.zip"] {
            assert!(plain_name(ok), "{ok}");
        }
        for bad in ["", ".", "..", ".modtopia", "a/b", "a\\b", "C:x", "../x"] {
            assert!(!plain_name(bad), "{bad:?}");
        }
    }

    #[test]
    fn an_empty_mods_folder_has_no_plan() {
        let dir = std::env::temp_dir().join(format!("recompia-mods-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(mods_dir(&dir)).unwrap();
        let v = list(&dir).unwrap();
        assert!(v.mods.is_empty() && v.points == 0);
        assert!(!dir.join(gamebuild::MOD_PLAN).exists());
        // the mappings went into the game folder
        assert!(MAPPINGS.iter().all(|(n, b)| fs::read(mappings_dir(&dir).join(n)).ok().as_deref() == Some(*b)));
        let _ = fs::remove_dir_all(&dir);
    }
}
