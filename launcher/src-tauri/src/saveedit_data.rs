// SPDX-License-Identifier: GPL-3.0-or-later
//! The save editor's game data: the names, tables and pictures PokoHex shows, made from the player's own game files, so the launcher
//! ships none of them. `tools/pokohex/pokohex-extract` (PokoHex.Extract, built by scripts/build-pokohex.sh) reads the romfs of
//! `[fs] romfs` and writes `<game>/pokohex_data/` once (about 2,200 small files, 35 MB, a few seconds); the Save Editor page serves
//! that folder to the editor through the `pokohex-data` protocol (`respond`, local_files.rs).
//!
//! The folder counts as ready while its manifest is the version the tool writes (EXPORT_VERSION) and `source.txt` names the romfs it
//! was made from: a new dump or a newer tool makes it again.

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Stdio;

use serde::Serialize;

/// The folder in the game folder that holds the editor's game data.
pub const DATA_DIR: &str = "pokohex_data";
/// The protocol the editor loads the folder through (http://pokohex-data.localhost/ on Windows, pokohex-data://localhost/ elsewhere).
pub const PROTOCOL: &str = "pokohex-data";
/// The manifest version pokohex-extract writes (GameDataExporter.Version); a folder of another version is made again.
const EXPORT_VERSION: u64 = 1;
const SOURCE: &str = "source.txt";

#[derive(Serialize, Clone, Debug)]
pub struct Progress {
    pub done: u64,
    pub total: u64,
}

pub fn data_dir(game_dir: &Path) -> PathBuf {
    game_dir.join(DATA_DIR)
}

/// The romfs the game plays, from `[fs] romfs` (relative paths are relative to the game folder).
fn romfs(game_dir: &Path) -> Result<PathBuf, String> {
    let value = crate::settings::read_pkp(game_dir).get("fs.romfs").map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
    let value = value.ok_or("The save editor needs the game files: choose them on the Set up page first.")?;
    let path = PathBuf::from(value);
    let path = if path.is_absolute() { path } else { game_dir.join(path) };
    if !path.is_dir() {
        return Err(format!("The game files are not where pkp.ini says ({}). Choose them again on the Set up page.", path.display()));
    }
    Ok(path)
}

/// True when the folder is complete, current and made from the romfs the game plays now.
pub fn is_ready(game_dir: &Path) -> bool {
    let dir = data_dir(game_dir);
    let manifest = std::fs::read(dir.join("manifest.json")).ok().and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok());
    let version_ok = manifest.as_ref().and_then(|m| m["Version"].as_u64()) == Some(EXPORT_VERSION);
    let source = std::fs::read_to_string(dir.join(SOURCE)).unwrap_or_default();
    version_ok && romfs(game_dir).is_ok_and(|r| source.trim() == r.to_string_lossy())
}

/// The extractor: `PKP_POKOHEX_EXTRACT`, else `tools/pokohex/` in the game folder, then next to the launcher.
fn extractor(game_dir: &Path) -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("PKP_POKOHEX_EXTRACT").map(PathBuf::from) {
        return Some(p);
    }
    let name = if cfg!(windows) { "pokohex-extract.exe" } else { "pokohex-extract" };
    [game_dir.join("tools"), crate::paths::exe_dir().join("tools")]
        .into_iter()
        .map(|t| t.join("pokohex").join(name))
        .find(|p| p.is_file())
}

/// Makes the folder from the romfs. `report` gets the pictures written so far and the total.
pub fn prepare(game_dir: &Path, mut report: impl FnMut(Progress)) -> Result<(), String> {
    let romfs = romfs(game_dir)?;
    let tool = extractor(game_dir).ok_or("This package has no save editor data tool (tools/pokohex). Extract the release zip again so tools/ is complete.")?;
    let dir = data_dir(game_dir);
    let mut cmd = crate::host::command(&tool);
    cmd.arg(&romfs).arg(&dir).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let mut child = cmd.spawn().map_err(|e| format!("could not start {}: {}", tool.display(), e))?;
    let mut err = child.stderr.take().unwrap();
    let err_text = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = err.read_to_string(&mut s);
        s
    });
    let mut error = None;
    for line in crate::child_lines::lines(child.stdout.take().unwrap()) {
        if let Some(rest) = line.strip_prefix("@progress ") {
            let mut it = rest.split(' ').map(|x| x.parse().unwrap_or(0));
            report(Progress { done: it.next().unwrap_or(0), total: it.next().unwrap_or(0) });
        } else if let Some(rest) = line.strip_prefix("@error ") {
            error = Some(rest.to_string());
        }
    }
    let status = child.wait().map_err(|e| e.to_string())?;
    let stderr = err_text.join().unwrap_or_default();
    if !status.success() {
        let why = error.or_else(|| stderr.lines().rev().find(|l| !l.trim().is_empty()).map(str::to_string)).unwrap_or_else(|| status.to_string());
        return Err(format!("The save editor could not read the game files: {why}"));
    }
    std::fs::write(dir.join(SOURCE), romfs.to_string_lossy().as_bytes()).map_err(|e| format!("cannot write {}: {}", dir.join(SOURCE).display(), e))?;
    Ok(())
}

/// The `pokohex-data` protocol: a file of the folder, read only. The editor fetches the text and tables from the launcher's own
/// origin, so the response allows any origin to read it (it holds nothing but the game data made from the player's files).
pub fn respond(game_dir: &Path, request: &tauri::http::Request<Vec<u8>>) -> tauri::http::Response<Vec<u8>> {
    crate::local_files::respond(&data_dir(game_dir), request)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ready_needs_the_manifest_version_and_the_source() {
        let game = std::env::temp_dir().join(format!("pkp-saveedit-data-{}", std::process::id()));
        let romfs = game.join("romfs");
        std::fs::create_dir_all(&romfs).unwrap();
        std::fs::write(game.join(crate::settings::PKP_INI), "[fs]\nromfs = romfs\n").unwrap();
        let dir = data_dir(&game);
        std::fs::create_dir_all(&dir).unwrap();
        assert!(!is_ready(&game));
        std::fs::write(dir.join("manifest.json"), r#"{"Version":1,"Text":[],"Byte":[],"Images":[]}"#).unwrap();
        assert!(!is_ready(&game), "no source");
        std::fs::write(dir.join(SOURCE), romfs.to_string_lossy().as_bytes()).unwrap();
        assert!(is_ready(&game));
        std::fs::write(dir.join("manifest.json"), r#"{"Version":2}"#).unwrap();
        assert!(!is_ready(&game), "another version");
        let _ = std::fs::remove_dir_all(&game);
    }
}
