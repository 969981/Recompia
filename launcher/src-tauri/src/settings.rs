// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads and edits the runtime's ini files next to pkp.exe. The key schema lives in the frontend;
//! this side only moves `section.key` values in and out, with the careful editor in ini.rs.

use crate::ini::Ini;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const PKP_INI: &str = "pkp.ini";
pub const CONTROLS_INI: &str = "controls.ini";

#[derive(Serialize)]
pub struct SettingsDump {
    pub game_dir: String,
    pub pkp_exists: bool,
    pub controls_exists: bool,
    pub pkp: HashMap<String, String>,
    pub controls: HashMap<String, String>,
}

#[derive(Deserialize)]
pub struct Edit {
    /// "pkp" or "controls"
    pub file: String,
    pub section: String,
    pub key: String,
    pub value: String,
}

fn load(path: &Path) -> Option<Ini> {
    let bytes = fs::read(path).ok()?;
    // the files are UTF-8 (controls.ini has a non-ASCII header); a stray bad byte must not lose the file
    Some(Ini::parse(&String::from_utf8_lossy(&bytes)))
}

fn map_of(ini: &Option<Ini>) -> HashMap<String, String> {
    ini.as_ref().map(|i| i.all().into_iter().collect()).unwrap_or_default()
}

pub fn file_for(dir: &Path, which: &str) -> Result<PathBuf, String> {
    match which {
        "pkp" => Ok(dir.join(PKP_INI)),
        "controls" => Ok(dir.join(CONTROLS_INI)),
        other => Err(format!("unknown settings file '{}'", other)),
    }
}

pub fn read(dir: &Path) -> SettingsDump {
    let pkp = load(&dir.join(PKP_INI));
    let controls = load(&dir.join(CONTROLS_INI));
    SettingsDump {
        game_dir: crate::dump::fwd(dir),
        pkp_exists: pkp.is_some(),
        controls_exists: controls.is_some(),
        pkp: map_of(&pkp),
        controls: map_of(&controls),
    }
}

fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    let tmp = path.with_extension("ini.tmp");
    fs::write(&tmp, text).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot replace {}: {}", path.display(), e)
    })
}

/// Applies edits grouped per file. controls.ini is never created: the game writes it with its factory
/// bindings on first run, and a file with only a few keys would lose them.
pub fn apply(dir: &Path, edits: &[Edit]) -> Result<(), String> {
    for which in ["pkp", "controls"] {
        let mine: Vec<&Edit> = edits.iter().filter(|e| e.file == which).collect();
        if mine.is_empty() {
            continue;
        }
        let path = file_for(dir, which)?;
        let mut ini = match load(&path) {
            Some(i) => i,
            None if which == "pkp" => Ini::empty(),
            None => return Err(format!("{} does not exist yet. Start the game once so it creates it.", CONTROLS_INI)),
        };
        for e in mine {
            ini.set(&e.section, &e.key, &e.value);
        }
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// Used by Play and setup: the ini keys the launcher owns. pkp.exe reads every path from pkp.ini (a pkp.ini next to it makes
/// the game folder its working directory), so it needs no command-line paths.
pub fn ensure_play_paths(dir: &Path, exefs: &str, romfs: &str) -> Result<(), String> {
    let path = dir.join(PKP_INI);
    let mut ini = load(&path).unwrap_or_else(Ini::empty);
    let mut changed = !path.exists();

    // [fs] exefs and [fs] romfs: kept while they name an existing folder (the user may have pointed them elsewhere)
    for (key, value) in [("exefs", exefs), ("romfs", romfs)] {
        let cur = ini.get("fs", key).unwrap_or_default();
        let cur_path = Path::new(cur.trim());
        // relative paths in the ini are relative to the game folder, the working directory of the game
        let cur_full = if cur_path.is_absolute() { cur_path.to_path_buf() } else { dir.join(cur_path) };
        if cur.trim().is_empty() || !cur_full.is_dir() {
            ini.set("fs", key, value);
            changed = true;
        }
    }
    if ini.get("nvn", "shader_cache").unwrap_or_default().trim() != crate::shaders::SPIRV_REL {
        ini.set("nvn", "shader_cache", crate::shaders::SPIRV_REL);
        changed = true;
    }
    // shaders the game builds while it runs, for play.rs to translate after it exits (kept if the user set it, even empty)
    if ini.get("nvn", "dump_missing").is_none() {
        ini.set("nvn", "dump_missing", crate::shaders::MISSING_REL);
        changed = true;
    }
    let cur_save = ini.get("fs", "save_dir").unwrap_or_default();
    if cur_save.trim().is_empty() {
        let save = dir.join("save");
        let _ = fs::create_dir_all(&save);
        ini.set("fs", "save_dir", &crate::dump::fwd(&save));
        changed = true;
    }
    if changed {
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn play_paths_turn_on_dump_missing_once() {
        let dir = std::env::temp_dir().join(format!("pkp_dm_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let d = crate::dump::fwd(&dir);
        ensure_play_paths(&dir, &d, &d).unwrap();
        assert_eq!(load(&dir.join(PKP_INI)).unwrap().get("nvn", "dump_missing").as_deref(), Some(crate::shaders::MISSING_REL));
        // the user turned it off: kept
        let mut ini = load(&dir.join(PKP_INI)).unwrap();
        ini.set("nvn", "dump_missing", "");
        write_atomic(&dir.join(PKP_INI), &ini.to_text()).unwrap();
        ensure_play_paths(&dir, &d, &d).unwrap();
        assert_eq!(load(&dir.join(PKP_INI)).unwrap().get("nvn", "dump_missing").as_deref(), Some(""));
        let _ = fs::remove_dir_all(&dir);
    }
}
