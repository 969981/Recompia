// SPDX-License-Identifier: GPL-3.0-or-later
//! Installing a mod from the hub's list, after the player's one yes (the page asks): the package is downloaded from the hub's download
//! server only (mods_catalog.rs `download_ok`; no redirect is followed), checked against the size and SHA-256 the list gives (a mismatch
//! stops the install), written as `<file>.part` in mods/ and renamed (download.rs), and nothing in it runs. Then the yes is recorded for
//! that exact file (the core's `mt_loader_trust_mod`: a later change to it asks again) and the mod is switched on. A newer version
//! installs the same way and replaces the older file.

use std::path::Path;
use std::sync::atomic::AtomicBool;

use crate::download::{self, Pinned};
use crate::mods::{self, ModsView};
use crate::mods_catalog::{self, CatalogStore};

pub fn install(game_dir: &Path, store: &CatalogStore, cache: &Path, id: &str) -> Result<ModsView, String> {
    let (catalog, offline) = mods_catalog::load(store, cache, false)?;
    if offline {
        return Err("Installing needs the mod hub, which cannot be reached now. Check the internet connection and try again.".into());
    }
    if catalog.preview {
        return Err("The mod hub is not open for installs yet.".into());
    }
    let entry = catalog.mods.iter().find(|e| e.id == id && mods_catalog::fits(e)).ok_or_else(|| format!("{} is not in the mod list for this game.", id))?;
    let dl = entry.download.as_ref().ok_or_else(|| format!("{} is not out yet.", entry.name))?;
    if !mods_catalog::download_ok(dl) {
        return Err(format!("{} cannot be installed: its download is not on the mod hub's server.", entry.name));
    }
    let dir = mods::mods_dir(game_dir);
    std::fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    let before = mods::installed(game_dir, id);
    let pin = Pinned { file: &dl.file, url: &dl.url, size: dl.size, sha256: &dl.sha256 };
    download::fetch(&download::agent(false), &pin, &dir, &AtomicBool::new(false), &mut |_| {})?;
    // the older version's file goes once the new one is in place (two copies of one id: the core loads only one)
    if let Some((_, source)) = before.filter(|(_, source)| *source != dl.file) {
        mods::delete_source(game_dir, &source)?;
    }
    mods::set_enabled(game_dir, id, true, true)
}
