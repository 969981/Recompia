// SPDX-License-Identifier: GPL-3.0-or-later
//! The first start after the launcher's file was renamed by the release rename (release_names.rs): once per install folder, and only
//! when this launcher runs under the new name from a real install (tools/build beside it), never under the old name, a name the player
//! gave it or from a development build.
//!  1. Steam: our shortcuts that start the old name are pointed at this file and keep their app id (steam::repoint_here). Never while
//!     Steam runs (it writes its own copy of the list back when it closes, and in a Steam Deck's Game Mode it always runs), and no
//!     retry: the small starter left under the old name keeps those shortcuts working, and Settings > Steam offers Update for them.
//!  2. The old name's leftovers in the install: `<old>.old` and versioned copies `<old>.<version>`. Never the old name itself (the
//!     starter: shortcuts the player made by hand start it), and nothing else (saves, settings, pkp.*).
//! Every step can run again without harm: a page that read launcher.json before the marker was set may write it back without it.

use std::fs;
use std::path::{Path, PathBuf};

use tauri::AppHandle;

use crate::config::{load_config, store_config};
use crate::release_names::{self, Host};
use crate::repair;
use crate::steam::{self, Repointed};

pub fn on_start(app: &AppHandle, launcher: &Path) {
    let Some(root) = launcher.parent() else { return };
    if release_names::is_new_name(launcher, Host::HERE) != Some(true) || !root.join("tools").join("build").is_dir() {
        return;
    }
    let here = root.to_string_lossy().into_owned();
    if load_config(app).renamed_in == here {
        return;
    }
    match steam::repoint_here() {
        Ok(Repointed::Accounts(0)) => {}
        Ok(Repointed::Accounts(n)) => eprintln!("rename: Steam shortcuts now start {} ({n} account(s))", launcher.display()),
        Ok(Repointed::SteamRunning) => eprintln!("rename: Steam runs, its shortcuts keep starting the old name until Update in Settings > Steam"),
        Err(e) => eprintln!("rename: Steam shortcuts: {e}"),
    }
    for p in leftovers(root, Host::HERE) {
        if let Err(e) = fs::remove_file(&p) {
            eprintln!("rename: {}: {e}", p.display());
        }
    }
    // read again: the page may have saved its own options meanwhile
    let mut cfg = load_config(app);
    cfg.renamed_in = here;
    if let Err(e) = store_config(app, &cfg) {
        eprintln!("rename: {e}");
    }
}

/// The old launcher name's leftovers in `root`: `<old>.old` and `<old>.<version>` (repair.rs is_copy_of), files only.
fn leftovers(root: &Path, host: Host) -> Vec<PathBuf> {
    let old = host.launchers()[1];
    let set_aside = format!("{old}.old");
    let Ok(rd) = fs::read_dir(root) else { return Vec::new() };
    let mut out: Vec<PathBuf> = rd
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
        .filter(|e| e.file_name().to_str().is_some_and(|n| n == set_aside || repair::is_copy_of(old, n)))
        .map(|e| e.path())
        .collect();
    out.sort();
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_old_names_set_aside_and_versioned_copies_are_leftovers() {
        let root = std::env::temp_dir().join(format!("recompia-rename-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("PokopiaLauncher.exe.0.9.0")).unwrap(); // a folder, not ours to delete
        for f in ["PokopiaLauncher.exe", "PokopiaLauncher.exe.old", "PokopiaLauncher.exe.0.14.2", "PokopiaLauncher.exe.bak", "Recompia.exe",
                  "Recompia.exe.0.15.0", "pkp.exe", "pkp.exe.0.1.0", "pkp.ini"] {
            fs::write(root.join(f), b"x").unwrap();
        }
        let got: Vec<String> = leftovers(&root, Host::Windows).iter().map(|p| p.file_name().unwrap().to_string_lossy().into_owned()).collect();
        assert_eq!(got, ["PokopiaLauncher.exe.0.14.2", "PokopiaLauncher.exe.old"]);
        for host in [Host::Linux, Host::MacOs] {
            let old = host.launchers()[1];
            for f in [old.to_string(), format!("{old}.old"), format!("{old}.0.14.2")] {
                fs::write(root.join(f), b"x").unwrap();
            }
            assert_eq!(leftovers(&root, host).len(), 2, "{host:?}: the starter under the old name is never one");
        }
        let _ = fs::remove_dir_all(&root);
    }
}
