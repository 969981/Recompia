// SPDX-License-Identifier: GPL-3.0-or-later
//! Validates a decrypted game dump folder. Read-only: nothing here writes to the dump.
//!
//! Expected layout: `<dump>/base/{exefs,romfs}` and `<dump>/updates/<version>/{exefs,romfs}`.
//! The highest valid update is used; with none, the base.

use serde::Serialize;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

const EXEFS_FILES: [&str; 5] = ["main", "sdk", "subsdk0", "rtld", "main.npdm"];

#[derive(Serialize, Clone)]
pub struct Check {
    pub label: String,
    pub ok: bool,
}

#[derive(Serialize, Clone)]
pub struct Candidate {
    /// "base" or "update"
    pub kind: String,
    /// Update version folder name; empty for the base.
    pub version: String,
    pub path: String,
    pub exefs: String,
    pub romfs: String,
    pub ok: bool,
    pub checks: Vec<Check>,
}

#[derive(Serialize, Clone)]
pub struct DumpReport {
    pub path: String,
    pub exists: bool,
    pub errors: Vec<String>,
    pub candidates: Vec<Candidate>,
    pub chosen: Option<Candidate>,
}

pub fn fwd(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

/// The build this release supports, from `tools/build/dump_hashes.json` next to the launcher: (main build ID, version text).
/// None in a dev checkout without the file: then any version passes here and the build script still checks the hashes.
fn supported() -> Option<(String, String)> {
    let p = std::env::current_exe().ok()?.parent()?.join("tools").join("build").join("dump_hashes.json");
    let v: serde_json::Value = serde_json::from_str(&fs::read_to_string(p).ok()?).ok()?;
    Some((v["main_build_id"].as_str()?.to_lowercase(), v["version"].as_str()?.to_string()))
}

/// Build ID of a decrypted NSO (20 bytes at 0x40), lower-case hex; None if the file is missing, short or not an NSO.
fn main_build_id(main: &Path) -> Option<String> {
    let mut h = [0u8; 0x54];
    fs::File::open(main).ok()?.read_exact(&mut h).ok()?;
    if &h[..4] != b"NSO0" {
        return None;
    }
    Some(h[0x40..0x54].iter().map(|b| format!("{:02x}", b)).collect())
}

fn check_candidate(kind: &str, version: &str, dir: &Path, want: Option<&(String, String)>) -> Candidate {
    let exefs = dir.join("exefs");
    let romfs = dir.join("romfs");
    let mut checks = Vec::new();
    for f in EXEFS_FILES {
        checks.push(Check { label: format!("exefs/{}", f), ok: exefs.join(f).is_file() });
    }
    checks.push(Check { label: "romfs/".to_string(), ok: romfs.is_dir() });
    // the right game version, up front: an older or newer update would otherwise be chosen and fail minutes into Set up
    if let Some((id, text)) = want {
        if exefs.join("main").is_file() {
            checks.push(Check { label: format!("version {}", text), ok: main_build_id(&exefs.join("main")).as_deref() == Some(id.as_str()) });
        }
    }
    let ok = checks.iter().all(|c| c.ok);
    Candidate {
        kind: kind.to_string(),
        version: version.to_string(),
        path: fwd(dir),
        exefs: fwd(&exefs),
        romfs: fwd(&romfs),
        ok,
        checks,
    }
}

/// Newest first: numeric versions by value, then anything non-numeric by name.
fn version_order(a: &str, b: &str) -> std::cmp::Ordering {
    match (a.parse::<u64>(), b.parse::<u64>()) {
        (Ok(x), Ok(y)) => y.cmp(&x),
        (Ok(_), Err(_)) => std::cmp::Ordering::Less,
        (Err(_), Ok(_)) => std::cmp::Ordering::Greater,
        (Err(_), Err(_)) => b.cmp(a),
    }
}

pub fn scan(path: &str) -> DumpReport {
    let root = PathBuf::from(path.trim());
    let mut report = DumpReport {
        path: fwd(&root),
        exists: root.is_dir(),
        errors: Vec::new(),
        candidates: Vec::new(),
        chosen: None,
    };
    if path.trim().is_empty() {
        report.errors.push("No folder chosen yet.".to_string());
        return report;
    }
    if !report.exists {
        report.errors.push("That folder does not exist or cannot be read.".to_string());
        return report;
    }
    let want = supported();
    let base = root.join("base");
    let updates = root.join("updates");
    if !base.is_dir() && !updates.is_dir() {
        report.errors.push(
            "This folder has no base/ or updates/ subfolder. Pick the folder that contains them (for example Pokopia_v2.0.1a).".to_string(),
        );
        return report;
    }

    if updates.is_dir() {
        let mut versions: Vec<String> = fs::read_dir(&updates)
            .map(|rd| {
                rd.filter_map(|e| e.ok())
                    .filter(|e| e.path().is_dir())
                    .map(|e| e.file_name().to_string_lossy().to_string())
                    .collect()
            })
            .unwrap_or_default();
        versions.sort_by(|a, b| version_order(a, b));
        for v in versions {
            report.candidates.push(check_candidate("update", &v, &updates.join(&v), want.as_ref()));
        }
    }
    if base.is_dir() {
        report.candidates.push(check_candidate("base", "", &base, want.as_ref()));
    }

    report.chosen = report.candidates.iter().find(|c| c.ok).cloned();
    if report.chosen.is_none() {
        if let Some((_, text)) = &want {
            if report.candidates.iter().any(|c| c.checks.iter().any(|k| k.label.starts_with("version ") && !k.ok)) {
                report.errors.push(format!("These game files are a different version. This release needs {}.", text));
                return report;
            }
        }
        report.errors.push(
            "No usable game files found. Each of base/ or an updates/<version>/ folder needs exefs/ (main, sdk, subsdk0, rtld, main.npdm) and romfs/."
                .to_string(),
        );
    }
    report
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_id_of_an_nso_header() {
        let dir = std::env::temp_dir().join(format!("pkp_dump_test_{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut h = vec![0u8; 0x60];
        h[..4].copy_from_slice(b"NSO0");
        h[0x40] = 0x76;
        h[0x53] = 0x0e;
        fs::write(dir.join("main"), &h).unwrap();
        assert_eq!(main_build_id(&dir.join("main")).unwrap(), format!("76{}0e", "00".repeat(18)));
        h[0] = b'X';
        fs::write(dir.join("main"), &h).unwrap();
        assert_eq!(main_build_id(&dir.join("main")), None);   // encrypted or not an NSO
        fs::write(dir.join("main"), b"NSO0").unwrap();
        assert_eq!(main_build_id(&dir.join("main")), None);   // short
        assert_eq!(main_build_id(&dir.join("nope")), None);
        let _ = fs::remove_dir_all(&dir);
    }
}
