// SPDX-License-Identifier: GPL-3.0-or-later
//! The mods the launcher carries itself: a package built into the launcher (`launcher/src-tauri/carried/`) that stands in for the mod
//! hub's copy of the same mod until the hub has a newer version. Today: Community Achievements 0.4.1 (the visited-place fix and
//! achievements.json), while the hub still has 0.4.0. Its source is the Modtopia repository's `catalog/community-achievements`.
//!
//! - Installing (`mods_install::install_best`): the carried package when the hub has no newer version (or cannot be reached), else the
//!   hub's. A tie goes to the carried one: the hub's copy of the same version may not hold the fix.
//! - Refreshing (`refresh`, once at launcher start): an installed copy older than the carried one is replaced by it, under the same rule
//!   as the hub update (mods_update.rs): only a file the player allowed (or one with no code), and the new file is allowed only when the
//!   old one had code the player allowed. A file dropped in by hand or changed since stays the player's.
//! - Once the hub has a newer version, the hub update and Browse take over as for any mod; nothing here goes back down.

use std::cmp::Ordering;
use std::path::Path;

use crate::fsutil::{self, AtomicFail};
use crate::modtopia_ffi as ffi;
use crate::mods;

/// One package the launcher carries. `version` is the one in its mod.toml (a test checks it).
pub struct Carried {
    pub id: &'static str,
    pub version: &'static str,
    /// its file name in mods/
    pub file: &'static str,
    bytes: &'static [u8],
}

const CARRIED: &[Carried] = &[Carried {
    id: crate::achievements::MOD_ID,
    version: "0.4.1",
    file: "community_achievements-0.4.1.pkmod",
    bytes: include_bytes!("../carried/community_achievements-0.4.1.pkmod"),
}];

pub fn find(id: &str) -> Option<&'static Carried> {
    CARRIED.iter().find(|c| c.id == id)
}

impl Carried {
    /// Whether this package is taken over the hub's version (None: the hub has none to offer now).
    pub fn wins_over(&self, hub_version: Option<&str>) -> bool {
        hub_version.is_none_or(|v| ffi::version_compare(v, self.version) != Ordering::Greater)
    }

    /// Writes the package into mods/ (whole or not at all), removes an older version's file, and, when `trust`, records the player's yes
    /// for its code (the same steps as mods_install.rs `put_in_place`).
    pub fn put_in_place(&self, game_dir: &Path, trust: bool) -> Result<(), String> {
        let dir = mods::ensure_mods_dir(game_dir)?;
        let before = mods::installed(game_dir, self.id);
        fsutil::write_atomic(&dir.join(self.file), self.bytes).map_err(|e| match e {
            AtomicFail::Write { tmp, error } => format!("Could not write {}: {error}", tmp.display()),
            AtomicFail::Replace { error } => format!("Could not put {} in place: {error}", self.file),
        })?;
        crate::mods_install::drop_older(game_dir, before, self.file)?;
        if trust {
            mods::trust_file(game_dir, self.id)?;
        }
        Ok(())
    }
}

/// Replaces installed copies older than the carried package (see the top). Returns the ids replaced; a failure is logged and the others
/// go on.
pub fn refresh(game_dir: &Path) -> Result<Vec<&'static str>, String> {
    let installed = mods::installed_all(game_dir);
    let mut done = Vec::new();
    for c in CARRIED {
        let Some(have) = installed.iter().find(|m| m.id == c.id) else { continue };
        if !have.allowed || ffi::version_compare(c.version, &have.version) != Ordering::Greater {
            continue;
        }
        match c.put_in_place(game_dir, have.code) {
            Ok(()) => {
                eprintln!("mods: {} {} replaced by the launcher's {}", c.id, have.version, c.version);
                done.push(c.id);
            }
            Err(e) => eprintln!("mods: {} not replaced by the launcher's {}: {e}", c.id, c.version),
        }
    }
    if !done.is_empty() {
        mods::refresh_plan(game_dir)?;
    }
    Ok(done)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mods::testkit::{pkmod, temp_game_dir};
    use std::fs;

    #[test]
    fn each_carried_package_is_the_mod_and_version_it_says() {
        for c in CARRIED {
            let dir = temp_game_dir(&format!("carried-{}", c.id));
            c.put_in_place(&dir, false).unwrap();
            assert_eq!(mods::installed(&dir, c.id), Some((c.version.to_string(), c.file.to_string())));
            let _ = fs::remove_dir_all(&dir);
        }
    }

    #[test]
    fn the_hub_wins_only_with_a_newer_version() {
        let c = find(crate::achievements::MOD_ID).unwrap();
        assert!(c.wins_over(None) && c.wins_over(Some("0.4.0")) && c.wins_over(Some(c.version)));
        assert!(!c.wins_over(Some("0.4.2")) && !c.wins_over(Some("1.0.0")));
    }

    #[test]
    fn an_older_allowed_copy_is_replaced_and_a_newer_one_kept() {
        let c = find(crate::achievements::MOD_ID).unwrap();
        let dir = temp_game_dir("carried-refresh");
        let mods_dir = mods::ensure_mods_dir(&dir).unwrap();
        fs::write(mods_dir.join("hub-copy.pkmod"), pkmod(c.id, "0.4.0", false)).unwrap();
        assert_eq!(refresh(&dir).unwrap(), [c.id]);
        assert_eq!(mods::installed(&dir, c.id), Some((c.version.to_string(), c.file.to_string())));
        assert!(!mods_dir.join("hub-copy.pkmod").exists(), "the older file goes");
        assert!(refresh(&dir).unwrap().is_empty(), "the same version: nothing");
        fs::remove_file(mods_dir.join(c.file)).unwrap();
        fs::write(mods_dir.join("newer.pkmod"), pkmod(c.id, "0.5.0", false)).unwrap();
        assert!(refresh(&dir).unwrap().is_empty(), "a newer copy (the hub's) stays");
        let _ = fs::remove_dir_all(&dir);
    }
}
