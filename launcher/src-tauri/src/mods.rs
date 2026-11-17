// SPDX-License-Identifier: GPL-3.0-or-later
//! The player's Modtopia mods in the game folder: the list the Mods page shows, its changes, and the hook plan Set up builds from.
//!
//! The Modtopia core owns the folders and files (contract 2: `<game>/mods/` with `modtopia.toml` and `.modtopia/`, `<game>/config/<id>.toml`,
//! `<game>/modtopia/mappings/`); this module reaches them only through it (modtopia_ffi.rs), never by editing them itself (the launcher's own
//! `mods/README.txt` is the one file it writes, `ensure_mods_dir`). Each call opens
//! a fresh scan, so a mod dropped into the folder by hand shows at the next look.
//!
//! Code the player has not allowed is held off in the effective list/plan without writing preferences. Enabling it asks for trust.
//! After every change the plan is written again (`refresh_plan`): `<game>/mods/.modtopia/hooks.tsv`, or no file when no mod needs a hook
//! point. Set up builds those points in; `pkp_build.py --check` compares the plan's hash, and readiness.rs says when Set up must run again.

use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};

use crate::gamebuild;
use crate::modtopia_ffi::{self as ffi, Core, ModInfo, SettingInfo};
pub use crate::mods_scope::Scope;

include!(concat!(env!("OUT_DIR"), "/modtopia_mappings.rs"));

pub fn mods_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("mods")
}

/// The one-screen help in `mods/`: what goes in, that code runs with the player's rights, where the switches are. English only; a plain
/// text file for someone who opens the folder in Explorer.
const README_NAME: &str = "README.txt";
const README: &str = concat!(
    "Put Modtopia mods here: .pkmod files, mod folders, or LayeredFS folders and zips.\r\n\r\n",
    "Use the launcher's Mods page to switch them on and off, put them in order, change their settings and find new ones. The game reads this folder each time it starts.\r\n\r\n",
    "A mod with code runs on this PC with your rights, like any program you install: only use mods from people you trust.\r\n",
);

/// The mods folder, made when missing, with README.txt when that is missing (never overwritten: the player may have changed or deleted it
/// on purpose, and a README that cannot be written does not stop the folder from being used).
pub fn ensure_mods_dir(game_dir: &Path) -> Result<PathBuf, String> {
    let dir = mods_dir(game_dir);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let readme = dir.join(README_NAME);
    if !readme.exists() {
        if let Err(e) = fs::write(&readme, README) {
            eprintln!("mods: {} not written: {}", readme.display(), e);
        }
    }
    Ok(dir)
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

fn scan(game_dir: &Path) -> Core {
    Core::open(&mods_dir(game_dir), &mappings_dir(game_dir), &host_name(), None)
}

/// Global configuration. Trusted-only planning holds unknown code off without modifying preferences.
fn open(game_dir: &Path) -> Core {
    if let Err(e) = write_mappings(game_dir) {
        eprintln!("mods: {e}");
    }
    scan(game_dir)
}

/// A scan with `scope`'s layer selected (mods_scope.rs): its switches and settings are the ones read and changed.
fn open_scope(game_dir: &Path, scope: &Scope) -> Result<Core, String> {
    let Some(account) = scope.account(game_dir)? else { return Ok(open(game_dir)); };
    write_mappings(game_dir)?;
    Ok(Core::open(&mods_dir(game_dir), &mappings_dir(game_dir), &host_name(), Some(&account)))
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
    let profiles = crate::profiles::load(game_dir);
    let core = open_scope(game_dir, &Scope::Profile(profiles.active_profile().id.clone()))?;
    write_plan(&core, game_dir).map(|_| ())
}

/// Refuses a start in `scope` (a co-op group) when a mod it has on needs a hook point pkp.exe was not built with. Set up builds the
/// active profile's plan only (refresh_plan), so a point only this scope needs is never built in, and the game would start that mod
/// switched off on this PC while the other members run it.
pub fn ensure_points_built(game_dir: &Path, scope: &Scope) -> Result<(), String> {
    let core = open_scope(game_dir, scope)?;
    if core.plan_count() == 0 {
        return Ok(());
    }
    let dir = game_dir.join(gamebuild::MOD_PLAN);
    let dir = dir.parent().unwrap();
    fs::create_dir_all(dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let tmp = crate::fsutil::temp_beside(&dir.join("scope-hooks.tsv"));
    let built = core.plan_write(&tmp).map_err(|_| format!("Cannot write {}.", tmp.display())).map(|()| gamebuild::plan_is_built(game_dir, &tmp));
    let _ = fs::remove_file(&tmp);
    if built? {
        Ok(())
    } else {
        Err(POINTS_NOT_BUILT.into())
    }
}

const POINTS_NOT_BUILT: &str = "A mod this co-op save has on needs hook points the game was not built with. Switch it off for this co-op save on the Mods page of Recompia: Co-op.";

/// One setting row of a mod.
#[derive(Serialize, Clone, Debug)]
pub struct SettingView {
    pub overridden: bool,
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
    /// kept on by the launcher until the player unlocks it (mods_lock.rs): it cannot be switched off
    pub locked: bool,
    /// one the launcher can lock: the page offers Unlock or Lock
    pub lockable: bool,
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
    SettingView { overridden: s.overridden, key: s.key, label: s.label, description: s.description, kind, restart: s.restart, min: s.min, max: s.max, step: s.step, choices: s.choices, text: s.text, number: s.number, locked: false, lockable: false }
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
    pub global_enabled: bool,
    pub enabled_override: Option<bool>,
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
    /// kept installed and on by the launcher until the player unlocks it (mods_lock.rs): it cannot be switched off or removed
    pub locked: bool,
    /// one the launcher can lock: the page offers Unlock or Lock
    pub lockable: bool,
}

/// The Mods page's list.
#[derive(Serialize, Clone, Debug)]
pub struct ModsView {
    /// Editing scope only; does not change the profile selected for Play. A singleplayer profile's id, or None.
    pub profile: Option<String>,
    /// The co-op group whose set this is (mods_scope.rs), or None.
    pub coop: Option<String>,
    /// the mods folder, for the page's "Open mods folder"
    pub dir: String,
    /// installed mods: those that load, in load order, then the rest
    pub mods: Vec<ModView>,
    /// "id@version" of every gameplay mod that is on (what the other Link Play players need too)
    pub gameplay: Vec<String>,
    /// hook points the mods that are on need
    pub points: i32,
    /// what the launcher locks now (mods_lock.rs `mark`; None until marked), so a page can offer Lock for a mod that is not installed
    pub locks: Option<crate::mods_lock::Locks>,
    /// the Community Achievements mod's id, so the page can say what its state means for the Achievements page
    pub achievements_mod: &'static str,
}

fn view_of(core: &Core, game_dir: &Path) -> ModsView {
    let mods = core
        .mods()
        .into_iter()
        .enumerate()
        .map(|(i, m)| ModView {
            gameplay: m.flags & ffi::FLAG_GAMEPLAY != 0,
            global_enabled: m.global_enabled,
            enabled_override: m.enabled_override,
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
            locked: false,
            lockable: false,
        })
        .collect();
    ModsView { profile: None, coop: None, dir: crate::dump::fwd(&mods_dir(game_dir)), mods, gameplay: core.gameplay_mods(), points: core.plan_count(), locks: None, achievements_mod: crate::achievements::MOD_ID }
}

/// The installed mods, after writing their plan (a mod dropped in by hand changes it as much as a click does).
pub fn list(game_dir: &Path) -> Result<ModsView, String> {
    list_in(game_dir, &Scope::Global)
}

/// The list as `scope` sees it. Editing another profile or a co-op group never replaces the active profile's build plan.
pub fn list_in(game_dir: &Path, scope: &Scope) -> Result<ModsView, String> {
    refresh_plan(game_dir)?;
    let core = open_scope(game_dir, scope)?;
    let mut view = view_of(&core, game_dir);
    view.profile = scope.profile_id();
    view.coop = scope.coop_id();
    Ok(view)
}

/// Opens a scan, applies `change` to it, then lists again with a fresh scan (the switches, the order and the plan are the scan's).
fn change_in(game_dir: &Path, scope: &Scope, change: impl FnOnce(&Core) -> Result<(), String>) -> Result<ModsView, String> {
    {
        let core = open_scope(game_dir, scope)?;
        change(&core)?;
    }
    list_in(game_dir, scope)
}

fn known(core: &Core, id: &str) -> Result<i32, String> {
    core.find(id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))
}

const SAVE_FAILED: &str = "Could not save the mods' list in the mods folder.";

/// The player's switch. A code mod that is not allowed yet is switched on only with `allow` (the player said yes to the question).
pub fn set_enabled(game_dir: &Path, id: &str, on: bool, allow: bool) -> Result<ModsView, String> {
    set_enabled_in(game_dir, id, on, allow, &Scope::Global, false)
}
pub fn set_enabled_in(game_dir: &Path, id: &str, on: bool, allow: bool, scope: &Scope, inherit: bool) -> Result<ModsView, String> {
    if inherit && scope.is_global() { return Err("Select a profile to remove an override.".into()); }
    change_in(game_dir, scope, |core| {
        known(core, id)?;
        let m = core.mods().into_iter().find(|m| m.id == id).unwrap();
        let effective = if inherit { m.global_enabled } else { on };
        if effective && needs_yes(&m) {
            if !allow {
                return Err("This mod's code is not allowed yet.".into());
            }
            core.trust_mod(id).map_err(|_| format!("Could not record your yes for {}.", m.name))?;
        }
        (if inherit { core.clear_enabled_override(id) } else { core.set_enabled(id, on) }).map_err(|_| SAVE_FAILED.to_string())
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
    set_setting_in(game_dir, id, key, number, text, &Scope::Global, false)
}
pub fn set_setting_in(game_dir: &Path, id: &str, key: &str, number: Option<f64>, text: Option<&str>, scope: &Scope, inherit: bool) -> Result<ModsView, String> {
    if inherit && scope.is_global() { return Err("Select a profile to remove an override.".into()); }
    change_in(game_dir, scope, |core| {
        let m = known(core, id)?;
        let i = core.settings(m).iter().position(|s| s.key == key).ok_or_else(|| format!("{} has no setting {}.", id, key))? as i32;
        let r = if inherit { core.clear_setting_override(m, i) } else { match (number, text) {
            (Some(v), _) => core.set_number(m, i, v),
            (None, Some(t)) => core.set_text(m, i, t),
            (None, None) => return Err(format!("No value for {}.", key)),
        } };
        r.map_err(|_| format!("Could not change {} of {}.", key, id))
    })
}

/// Clear the exceptions of every profile and co-op group when the existing global lock policy is re-applied.
pub fn clear_locked_overrides(game_dir: &Path, id: &str, enabled: bool, setting: Option<&str>) -> Result<(), String> {
    for scope in Scope::every_layer(game_dir) {
        let core = open_scope(game_dir, &scope)?;
        let Some(m) = core.find(id) else { continue; };
        if enabled { core.clear_enabled_override(id).map_err(|_| SAVE_FAILED.to_string())?; }
        if let Some(key) = setting {
            if let Some(i) = core.settings(m).iter().position(|s| s.key == key) {
                core.clear_setting_override(m, i as i32).map_err(|_| SAVE_FAILED.to_string())?;
            }
        }
    }
    refresh_plan(game_dir)
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

/// One installed mod as the hub update looks at it.
#[derive(Clone, Debug, PartialEq)]
pub struct Installed {
    pub id: String,
    pub name: String,
    pub version: String,
    /// no code to run, or code the player has said yes to (its file is the one that was allowed)
    pub allowed: bool,
    /// the installed file has code (with `allowed`: the player said yes to code for this mod)
    pub code: bool,
}

/// Every installed mod with its version and whether its file is allowed.
pub fn installed_all(game_dir: &Path) -> Vec<Installed> {
    open(game_dir)
        .mods()
        .into_iter()
        .map(|m| Installed { allowed: !needs_yes(&m), code: m.flags & ffi::FLAG_CODE != 0, id: m.id, name: m.name, version: m.version })
        .collect()
}

/// Records the player's yes for the file now in mods/ under this id, when it has code that is not allowed yet (an install or update that
/// came through the hub's checked download). Opens the folder without `open`'s hold-back, which would switch the mod off first.
pub fn trust_file(game_dir: &Path, id: &str) -> Result<(), String> {
    let core = scan(game_dir);
    let m = core.mods().into_iter().find(|m| m.id == id).ok_or_else(|| format!("There is no mod {} in the mods folder.", id))?;
    if !needs_yes(&m) {
        return Ok(());
    }
    core.trust_mod(id).map_err(|_| format!("Could not record your yes for {}.", m.name))
}

/// Test helpers: a game folder and .pkmod packages made in memory (stored zips), so the real core can scan them.
#[cfg(test)]
pub(crate) mod testkit {
    use std::path::PathBuf;

    pub fn temp_game_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("recompia-mods-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn crc32(data: &[u8]) -> u32 {
        let mut crc = !0u32;
        for b in data {
            crc ^= *b as u32;
            for _ in 0..8 {
                crc = if crc & 1 != 0 { crc >> 1 ^ 0xEDB8_8320 } else { crc >> 1 };
            }
        }
        !crc
    }

    /// A zip with every file stored (no compression), which is all the core's reader needs for a test.
    fn zip_stored(files: &[(&str, &[u8])]) -> Vec<u8> {
        let (mut out, mut central) = (Vec::new(), Vec::new());
        let le = |v: &mut Vec<u8>, n: u64, bytes: usize| v.extend_from_slice(&n.to_le_bytes()[..bytes]);
        for (name, data) in files {
            let (crc, offset, len, nlen) = (crc32(data) as u64, out.len() as u64, data.len() as u64, name.len() as u64);
            for (n, w) in [(0x0403_4b50, 4), (20, 2), (0, 2), (0, 2), (0, 2), (0, 2), (crc, 4), (len, 4), (len, 4), (nlen, 2), (0, 2)] {
                le(&mut out, n, w);
            }
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(data);
            for (n, w) in [(0x0201_4b50, 4), (20, 2), (20, 2), (0, 2), (0, 2), (0, 2), (0, 2), (crc, 4), (len, 4), (len, 4), (nlen, 2), (0, 2), (0, 2), (0, 2), (0, 2), (0, 4), (offset, 4)] {
                le(&mut central, n, w);
            }
            central.extend_from_slice(name.as_bytes());
        }
        let (cd_off, cd_size, count) = (out.len() as u64, central.len() as u64, files.len() as u64);
        out.extend_from_slice(&central);
        for (n, w) in [(0x0605_4b50, 4), (0, 2), (0, 2), (count, 2), (count, 2), (cd_size, 4), (cd_off, 4), (0, 2)] {
            le(&mut out, n, w);
        }
        out
    }

    /// A .pkmod: a mod.toml with this id and version, and when `code` a Windows library (the core only looks at it, nothing runs).
    pub fn pkmod(id: &str, version: &str, code: bool) -> Vec<u8> {
        let toml = format!("id = \"{id}\"\nversion = \"{version}\"\nname = \"{id}\"\nauthors = [\"Test\"]\nlicense = \"MIT\"\n");
        let mut files: Vec<(&str, &[u8])> = vec![("mod.toml", toml.as_bytes())];
        if code {
            files.push(("bin/windows-x64/mod.dll", b"MZ"));
        }
        zip_stored(&files)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn override_fixture(name: &str) -> (PathBuf, String, String) {
        let dir = testkit::temp_game_dir(name);
        let folder = mods_dir(&dir).join("demo");
        fs::create_dir_all(&folder).unwrap();
        // No manifest opt-in: all mods inherit and may be overridden.
        fs::write(folder.join("mod.toml"), "id = \"demo\"\nversion = \"1.0.0\"\n\
            [[settings]]\nkey = \"server\"\ntype = \"text\"\nlabel = \"Server\"\ndefault = \"factory\"\n\
            [[settings]]\nkey = \"switch\"\ntype = \"bool\"\nlabel = \"Switch\"\ndefault = true\n\
            [[settings]]\nkey = \"level\"\ntype = \"number\"\nlabel = \"Level\"\ndefault = 2\nmin = 0\nmax = 10\nstep = 1\n\
            [[settings]]\nkey = \"choice\"\ntype = \"choice\"\nlabel = \"Choice\"\nchoices = [\"low\", \"high\"]\ndefault = \"low\"\n").unwrap();
        let a = crate::profiles::add(&dir, "First").unwrap().profiles.last().unwrap().id.clone();
        let b = crate::profiles::add(&dir, "Second").unwrap().profiles.last().unwrap().id.clone();
        (dir, a, b)
    }
    fn p(id: &str) -> Scope {
        Scope::Profile(id.to_string())
    }
    fn mod_view(dir: &Path, scope: &Scope) -> ModView {
        list_in(dir, scope).unwrap().mods.into_iter().find(|m| m.id == "demo").unwrap()
    }
    fn profile_folder(dir: &Path, id: &str) -> PathBuf {
        let profiles = crate::profiles::load(dir);
        dir.join("config/accounts").join(crate::saveimport::account_key_of(&profiles.get(id).unwrap().uid))
    }

    #[test]
    fn sparse_overrides_inherit_global_changes_and_reset_to_global() {
        let (dir, a, b) = override_fixture("inherit-settings");
        fs::create_dir_all(dir.join("config/demo")).unwrap();
        fs::write(dir.join("config/demo/journal"), "keep").unwrap();
        set_setting(&dir, "demo", "server", None, Some("global-one")).unwrap();
        assert_eq!(mod_view(&dir, &p(&a)).settings[0].text, "global-one");
        assert!(!profile_folder(&dir, &a).exists(), "reading inheritance must not materialize overrides");
        let globals = fs::read(dir.join("config/demo.toml")).unwrap();
        let active = crate::profiles::load(&dir).active;
        set_setting_in(&dir, "demo", "server", None, Some("first"), &p(&a), false).unwrap();
        // An explicit same-value override pins the value too.
        set_setting_in(&dir, "demo", "server", None, Some("global-one"), &p(&b), false).unwrap();
        assert_eq!(fs::read(dir.join("config/demo.toml")).unwrap(), globals);
        let sparse = fs::read_to_string(profile_folder(&dir, &a).join("demo.toml")).unwrap();
        assert!(!sparse.contains("switch =") && !sparse.contains("level =") && !sparse.contains("choice ="));
        set_setting(&dir, "demo", "server", None, Some("global-two")).unwrap();
        assert_eq!(mod_view(&dir, &p(&a)).settings[0].text, "first");
        assert_eq!(mod_view(&dir, &p(&b)).settings[0].text, "global-one");
        assert_eq!(mod_view(&dir, &p("default")).settings[0].text, "global-two");
        set_setting_in(&dir, "demo", "server", None, None, &p(&b), true).unwrap();
        assert!(!mod_view(&dir, &p(&b)).settings[0].overridden);
        set_setting(&dir, "demo", "server", None, Some("global-three")).unwrap();
        assert_eq!(mod_view(&dir, &p(&b)).settings[0].text, "global-three");
        for (key, value) in [("switch", 0.0), ("level", 7.0), ("choice", 1.0)] {
            set_setting_in(&dir, "demo", key, Some(value), None, &p(&a), false).unwrap();
        }
        let m = mod_view(&dir, &p(&a));
        assert_eq!((m.settings[1].number, m.settings[2].number, m.settings[3].text.as_str()), (0.0, 7.0, "high"));
        for key in ["switch", "level", "choice"] { set_setting_in(&dir, "demo", key, None, None, &p(&a), true).unwrap(); }
        assert!(mod_view(&dir, &p(&a)).settings[1..].iter().all(|s| !s.overridden));
        crate::profiles::rename(&dir, &a, "Renamed").unwrap();
        assert_eq!(mod_view(&dir, &p(&a)).settings[0].text, "first");
        assert_eq!(crate::profiles::load(&dir).active, active, "editing context is not playing context");
        assert_eq!(fs::read_to_string(dir.join("config/demo/journal")).unwrap(), "keep");
        assert!(list_in(&dir, &p("../invalid")).is_err());
        assert!(set_setting_in(&dir, "demo", "server", None, None, &Scope::Global, true).is_err());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn enabled_overrides_support_both_directions_and_inheritance() {
        let (dir, a, b) = override_fixture("inherit-enabled");
        set_enabled(&dir, "demo", true, false).unwrap();
        let global = fs::read(dir.join("mods/modtopia.toml")).unwrap();
        set_enabled_in(&dir, "demo", false, false, &p(&a), false).unwrap();
        assert_eq!(mod_view(&dir, &p(&a)).enabled_override, Some(false));
        assert!(mod_view(&dir, &p(&b)).enabled);
        assert_eq!(fs::read(dir.join("mods/modtopia.toml")).unwrap(), global);
        set_enabled(&dir, "demo", false, false).unwrap();
        assert!(!mod_view(&dir, &p(&b)).enabled);
        set_enabled_in(&dir, "demo", true, false, &p(&b), false).unwrap();
        assert!(mod_view(&dir, &p(&b)).enabled && !mod_view(&dir, &Scope::Global).enabled);
        set_enabled_in(&dir, "demo", false, false, &p(&b), true).unwrap();
        assert_eq!(mod_view(&dir, &p(&b)).enabled_override, None);
        assert!(!mod_view(&dir, &p(&b)).enabled);
        set_enabled(&dir, "demo", true, false).unwrap();
        assert!(!mod_view(&dir, &p(&a)).enabled && mod_view(&dir, &p(&b)).enabled);
        set_enabled_in(&dir, "demo", false, false, &p(&a), true).unwrap();
        assert!(mod_view(&dir, &p(&a)).enabled);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_co_op_group_has_a_layer_of_its_own() {
        let (dir, a, _) = override_fixture("coop-layer");
        let g = crate::coop::create(&crate::coop::root_of(&dir), "Island", "Ash", "192.0.2.10").unwrap();
        let group = Scope::Coop(g.id.clone());
        set_setting_in(&dir, "demo", "server", None, Some("group"), &group, false).unwrap();
        set_enabled_in(&dir, "demo", false, false, &group, false).unwrap();
        let v = list_in(&dir, &group).unwrap();
        assert_eq!((v.coop.as_deref(), v.profile.as_deref()), (Some(g.id.as_str()), None));
        assert_eq!(mod_view(&dir, &group).settings[0].text, "group");
        assert!(!mod_view(&dir, &group).enabled);
        assert_eq!(mod_view(&dir, &p(&a)).settings[0].text, "factory", "the singleplayer profile keeps its own");
        assert!(mod_view(&dir, &p(&a)).enabled && mod_view(&dir, &Scope::Global).enabled);
        // the layer the game picks from NXRT_ACCOUNT_UID (coop_launch.rs)
        assert!(dir.join("config/accounts").join(crate::saveimport::account_key_of(&g.account_uid)).join("demo.toml").exists());
        // the folder Scope::layer_dir names (what deleting a co-op save removes) is the one the core wrote
        assert!(group.layer_dir(&dir).unwrap().unwrap().join("demo.toml").exists());
        ensure_points_built(&dir, &group).unwrap();
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_co_op_group_starts_only_with_its_hook_points_built() {
        let (dir, _, _) = override_fixture("coop-points");
        let folder = mods_dir(&dir).join("demo");
        let binary = folder.join("bin").join(ffi::platform_tag());
        fs::create_dir_all(&binary).unwrap();
        fs::write(binary.join(if cfg!(windows) { "mod.dll" } else if cfg!(target_os = "macos") { "mod.dylib" } else { "mod.so" }), b"fixture-code").unwrap();
        let mut manifest = fs::read_to_string(folder.join("mod.toml")).unwrap();
        manifest.push_str("\n[[hooks.\"2.0.1\"]]\nname = \"probe\"\ntarget = \"main+0x100\"\nword = 0xd503201f\nkind = \"observe\"\n");
        fs::write(folder.join("mod.toml"), manifest).unwrap();
        set_enabled(&dir, "demo", false, false).unwrap();
        let g = crate::coop::create(&crate::coop::root_of(&dir), "Island", "Ash", "192.0.2.10").unwrap();
        let group = Scope::Coop(g.id);
        set_enabled_in(&dir, "demo", true, true, &group, false).unwrap();
        assert_eq!(ensure_points_built(&dir, &group), Err(POINTS_NOT_BUILT.to_string()), "only the group has it on: never built in");
        let built = dir.join(gamebuild::MOD_HOOKS_BUILT);
        fs::create_dir_all(built.parent().unwrap()).unwrap();
        fs::write(&built, "# modtopia hooks 1\n# hash x\nmain\t0x100\tobserve\t0xd503201f\tmidhook\n").unwrap();
        ensure_points_built(&dir, &group).unwrap();
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn profile_plans_follow_play_selection_and_trust_cannot_be_inherited() {
        let (dir, a, b) = override_fixture("inherit-plans");
        let folder = mods_dir(&dir).join("demo");
        let binary = folder.join("bin").join(ffi::platform_tag());
        fs::create_dir_all(&binary).unwrap();
        let library = binary.join(if cfg!(windows) { "mod.dll" } else if cfg!(target_os = "macos") { "mod.dylib" } else { "mod.so" });
        fs::write(&library, b"fixture-code-one").unwrap();
        let mut manifest = fs::read_to_string(folder.join("mod.toml")).unwrap();
        manifest.push_str("\n[[hooks.\"2.0.1\"]]\nname = \"probe\"\ntarget = \"main+0x100\"\nword = 0xd503201f\nkind = \"observe\"\n");
        fs::write(folder.join("mod.toml"), manifest).unwrap();
        set_enabled(&dir, "demo", false, false).unwrap();
        assert!(set_enabled_in(&dir, "demo", true, false, &p(&b), false).is_err());
        set_enabled_in(&dir, "demo", true, true, &p(&b), false).unwrap();
        assert!(!dir.join(gamebuild::MOD_PLAN).exists(), "editing inactive profile must not change the active plan");
        crate::profiles::select(&dir, &b).unwrap(); refresh_plan(&dir).unwrap();
        let plan = fs::read(dir.join(gamebuild::MOD_PLAN)).unwrap();
        assert!(String::from_utf8_lossy(&plan).contains("0x100"));
        set_enabled_in(&dir, "demo", false, false, &p(&a), false).unwrap();
        list(&dir).unwrap();
        assert_eq!(fs::read(dir.join(gamebuild::MOD_PLAN)).unwrap(), plan);
        assert_eq!(crate::profiles::load(&dir).active, b);
        set_enabled(&dir, "demo", true, false).unwrap();
        let global = fs::read(dir.join("mods/modtopia.toml")).unwrap();
        let overrides = fs::read(profile_folder(&dir, &b).join(".enabled.toml")).unwrap();
        fs::write(&library, b"fixture-code-two").unwrap();
        assert!(mod_view(&dir, &p(&b)).needs_yes);
        assert_eq!(fs::read(dir.join("mods/modtopia.toml")).unwrap(), global);
        assert_eq!(fs::read(profile_folder(&dir, &b).join(".enabled.toml")).unwrap(), overrides);
        assert!(set_enabled_in(&dir, "demo", true, false, &p(&b), true).is_err());
        set_enabled_in(&dir, "demo", true, true, &p(&b), true).unwrap();
        assert_eq!(mod_view(&dir, &p(&b)).enabled_override, None);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn damaged_profile_overrides_are_preserved_and_write_failures_do_not_change_globals() {
        let (dir, a, _) = override_fixture("inherit-damage");
        fs::create_dir_all(mods_dir(&dir).join("dependent")).unwrap();
        fs::write(mods_dir(&dir).join("dependent/mod.toml"), "id = \"dependent\"\nversion = \"1.0.0\"\n[[dependencies]]\nid = \"demo\"\nversion = \">=1.0.0\"\nrequired = true\n").unwrap();
        set_setting(&dir, "demo", "server", None, Some("global")).unwrap();
        let path = profile_folder(&dir, &a);
        fs::create_dir_all(path.join("demo.toml")).unwrap();
        assert!(set_setting_in(&dir, "demo", "server", None, Some("new"), &p(&a), false).is_err());
        fs::remove_dir(path.join("demo.toml")).unwrap();
        fs::write(path.join("demo.toml"), "server = [broken").unwrap();
        assert_eq!(mod_view(&dir, &p(&a)).state, "error");
        assert_eq!(list_in(&dir, &p(&a)).unwrap().mods.iter().find(|m| m.id == "dependent").unwrap().state, "error");
        assert!(set_setting_in(&dir, "demo", "server", None, Some("new"), &p(&a), false).is_err());
        assert_eq!(fs::read_to_string(path.join("demo.toml")).unwrap(), "server = [broken");
        fs::write(path.join(".enabled.toml"), "[enabled]\ndemo = \"off\"").unwrap();
        assert!(set_enabled_in(&dir, "demo", true, false, &p(&a), false).is_err());
        assert_eq!(mod_view(&dir, &Scope::Global).settings[0].text, "global");
        let _ = fs::remove_dir_all(dir);
    }

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
    fn an_install_records_the_yes_for_that_one_mod_only() {
        use testkit::{pkmod, temp_game_dir};
        let dir = temp_game_dir("trust");
        let mods = ensure_mods_dir(&dir).unwrap();
        for (file, id, code) in [("cam-1.0.0.pkmod", "cam", true), ("cam2-1.0.0.pkmod", "cam2", true), ("look-1.0.0.pkmod", "look", false)] {
            fs::write(mods.join(file), pkmod(id, "1.0.0", code)).unwrap();
        }
        let ask = |dir: &Path, id: &str| list(dir).unwrap().mods.into_iter().find(|m| m.id == id).unwrap().needs_yes;
        assert!(ask(&dir, "cam") && ask(&dir, "cam2") && !ask(&dir, "look"), "code is new until the player said yes; data has none to ask for");
        trust_file(&dir, "cam").unwrap();
        assert!(!ask(&dir, "cam") && ask(&dir, "cam2"), "the yes is for cam, not for the code beside it");
        trust_file(&dir, "look").unwrap();
        let known = fs::read_to_string(mods.join(".modtopia").join("known-code.toml")).unwrap();
        assert!(known.contains("cam") && !known.contains("cam2") && !known.contains("look"), "{known}");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_mods_folder_gets_a_readme_that_is_never_overwritten() {
        let dir = std::env::temp_dir().join(format!("recompia-mods-readme-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let readme = ensure_mods_dir(&dir).unwrap().join(README_NAME);
        let first = fs::read_to_string(&readme).unwrap();
        assert!(first.contains("Mods page") && first.contains("your rights") && first.contains("\r\n"), "{first:?}");
        fs::write(&readme, "mine").unwrap();
        ensure_mods_dir(&dir).unwrap();
        assert_eq!(fs::read_to_string(&readme).unwrap(), "mine", "an existing README is left as it is");
        fs::remove_file(&readme).unwrap();
        ensure_mods_dir(&dir).unwrap();
        assert_eq!(fs::read_to_string(&readme).unwrap(), first, "a missing one is written again");
        assert!(list(&dir).unwrap().mods.is_empty(), "the README is not a mod");
        let _ = fs::remove_dir_all(&dir);
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
