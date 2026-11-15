// SPDX-License-Identifier: GPL-3.0-or-later
//! The save editor's file access (the Save Editor page embeds PokoHex, a save editor that runs in the page). Reading hands the active
//! profile's save to the page as files; writing puts the edited files back as that save.
//!
//! Only the game's own save parts travel: `G00/` and `S00/` (what PokoHex reads and writes). A write never deletes a file: it copies the
//! live folder beside it, writes the edited files over that copy, makes a before-edit safety copy (savebackup.rs) and swaps the copy in
//! the way a restore does (savebackup::swap_in), so anything else in the folder (B00) stays, and a failure part way leaves the save as it
//! was. The caller checks that the game is closed.

use std::fs;
use std::path::{Component, Path};

use base64::Engine as _;
use serde::{Deserialize, Serialize};

use crate::saveimport::{is_save_folder, locate};
use crate::savebackup::{self, Entry, KIND_EDIT};

/// The save parts the editor may read and write.
const PARTS: [&str; 2] = ["G00", "S00"];
/// What one save may hold at most (the runtime's account save is 64 MiB with its journal; this leaves room).
const MAX_BYTES: u64 = 128 * 1024 * 1024;
const MAX_FILES: usize = 512;

/// One file of the save: its path below the account folder with `/` between parts (`G00/CMNDAT.BIN`) and its bytes as base64.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct SaveFile {
    pub path: String,
    pub data: String,
}

#[derive(Serialize, Debug)]
pub struct Written {
    /// how many files were written
    pub files: usize,
    /// the copy of the save as it was just before the write (Settings > Saves > Backups can put it back)
    pub safety: Option<Entry>,
}

/// The active profile's save as files, sorted by path.
pub fn read(game_dir: &Path) -> Result<Vec<SaveFile>, String> {
    let (sd, key) = locate(game_dir);
    let root = sd.join("account").join(&key);
    if !is_save_folder(&root) {
        return Err("There is no save to edit yet. Play the game once, or import a save in Settings, then try again.".into());
    }
    let mut out = Vec::new();
    let mut total = 0u64;
    for part in PARTS {
        collect(&root.join(part), part, &mut out, &mut total)?;
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

fn collect(dir: &Path, rel: &str, out: &mut Vec<SaveFile>, total: &mut u64) -> Result<(), String> {
    let rd = fs::read_dir(dir).map_err(|e| format!("Cannot read {}: {}", dir.display(), e))?;
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let path = format!("{}/{}", rel, name);
        let md = e.metadata().map_err(|e| e.to_string())?;
        if md.is_dir() {
            collect(&e.path(), &path, out, total)?;
        } else if md.is_file() {
            *total += md.len();
            if *total > MAX_BYTES || out.len() >= MAX_FILES {
                return Err("The save is larger than the editor can open.".into());
            }
            let bytes = fs::read(e.path()).map_err(|e| format!("Cannot read {}: {}", path, e))?;
            out.push(SaveFile { path, data: base64::engine::general_purpose::STANDARD.encode(bytes) });
        }
    }
    Ok(())
}

/// A path the editor may write: relative, inside G00/ or S00/, plain names only (no `..`, no drive, no empty part).
fn valid_path(path: &str) -> bool {
    let p = Path::new(path);
    let parts: Vec<_> = path.split('/').collect();
    parts.len() >= 2
        && PARTS.contains(&parts[0])
        && parts.iter().all(|s| !s.is_empty() && *s != "." && *s != ".." && !s.contains(['\\', ':', '\0']))
        && p.components().all(|c| matches!(c, Component::Normal(_)))
}

/// Decodes and checks the files before anything is touched.
fn decode(files: &[SaveFile]) -> Result<Vec<(String, Vec<u8>)>, String> {
    if files.is_empty() || files.len() > MAX_FILES {
        return Err("The editor sent no save to write.".into());
    }
    let mut out = Vec::with_capacity(files.len());
    let mut total = 0u64;
    for f in files {
        if !valid_path(&f.path) {
            return Err(format!("The editor sent a file the save cannot hold: {}", f.path));
        }
        let bytes = base64::engine::general_purpose::STANDARD.decode(&f.data).map_err(|_| format!("The editor sent a damaged file: {}", f.path))?;
        total += bytes.len() as u64;
        if total > MAX_BYTES {
            return Err("The edited save is larger than the game's save can hold.".into());
        }
        out.push((f.path.clone(), bytes));
    }
    if !out.iter().any(|(p, _)| p == "G00/CMNDAT.BIN") {
        return Err("The editor sent a save without G00/CMNDAT.BIN.".into());
    }
    Ok(out)
}

/// Writes the edited files over the active profile's save (see the module comment). The game must be closed (the caller checks).
pub fn write(game_dir: &Path, files: &[SaveFile]) -> Result<Written, String> {
    let decoded = decode(files)?;
    let (sd, key) = locate(game_dir);
    let acc = sd.join("account");
    let live = acc.join(&key);
    if !is_save_folder(&live) {
        return Err("There is no save to write to. Play the game once, or import a save in Settings, then try again.".into());
    }
    let incoming = acc.join(format!("{}.edit-{}", key, savebackup::swap_stamp()));
    let _ = fs::remove_dir_all(&incoming);
    let staged = (|| -> std::io::Result<()> {
        crate::fsutil::copy_tree(&live, &incoming)?;
        for (rel, bytes) in &decoded {
            let to = incoming.join(rel);
            if let Some(dir) = to.parent() {
                fs::create_dir_all(dir)?;
            }
            fs::write(&to, bytes)?;
        }
        Ok(())
    })();
    if let Err(e) = staged {
        let _ = fs::remove_dir_all(&incoming);
        return Err(format!("Cannot prepare the edited save: {}. Your save was not changed.", e));
    }
    let safety = match savebackup::safety_copy(game_dir, KIND_EDIT) {
        Ok(s) => s,
        Err(e) => {
            let _ = fs::remove_dir_all(&incoming);
            return Err(e);
        }
    };
    savebackup::swap_in(&sd, &key, &incoming, None)?;
    Ok(Written { files: decoded.len(), safety })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths() {
        assert!(valid_path("G00/CMNDAT.BIN"));
        assert!(valid_path("S00/sub/SYDAT.BIN"));
        for bad in ["", "G00", "B00/x", "../G00/x", "G00/../x", "G00//x", "G00/./x", "/G00/x", "G00/a\\b", "G00/C:x", "g00/x"] {
            assert!(!valid_path(bad), "{bad}");
        }
    }

    #[test]
    fn decode_needs_the_common_file() {
        let enc = |s: &[u8]| base64::engine::general_purpose::STANDARD.encode(s);
        assert!(decode(&[SaveFile { path: "S00/SYDAT.BIN".into(), data: enc(b"x") }]).is_err());
        assert!(decode(&[SaveFile { path: "G00/CMNDAT.BIN".into(), data: "!!".into() }]).is_err());
        let ok = decode(&[SaveFile { path: "G00/CMNDAT.BIN".into(), data: enc(b"abc") }]).unwrap();
        assert_eq!(ok, vec![("G00/CMNDAT.BIN".to_string(), b"abc".to_vec())]);
    }
}
