// SPDX-License-Identifier: GPL-3.0-or-later
//! Recompia's additions to the mod hub's list: mods sent to the hub that it has not listed yet, in `carried/hub-additions.json` (the
//! hub's entry format, plus an optional `platforms` list). They are added to the hub's list where it is read (mods_catalog.rs `load`),
//! so Browse, Install and the update treat them like the hub's own mods.
//!
//! - An addition is shown only while the hub's list has no mod with its id: once the hub lists it, the hub's entry is the only one and
//!   ours drops out by itself (an installed copy then takes the hub's updates as any mod does).
//! - Each addition names one exact file (its release asset on GitHub, never a "latest" link) with its size and SHA-256; the download is
//!   taken only when it matches. Only those exact addresses are allowed besides the hub's server (`is_addition`), so the hub's list can
//!   never point somewhere new through this.
//! - A mod whose content the project should not hand out itself (media taken from elsewhere) is listed with `manual`, its author's page,
//!   and no download: Browse explains why and opens that page, and the player puts the file into mods/ themselves.
//! - Adding a mod: append its entry (read from the package's own mod.toml: id, version, game, loader as `needs`), size and SHA-256 of the
//!   exact file; drop it once the hub lists it.

use std::sync::LazyLock;

use serde::Deserialize;

use crate::modtopia_ffi as ffi;
use crate::mods_catalog::{Catalog, Download, Entry};

#[derive(Deserialize)]
struct Addition {
    #[serde(flatten)]
    entry: Entry,
    /// the platforms its package has code for (absent: every platform, e.g. a data-only mod)
    #[serde(default)]
    platforms: Option<Vec<String>>,
    /// a mod the launcher lists but does not install for the player (its content's rights are unclear): no `download`, and this page of
    /// its author's, where the player gets it themselves (`Entry::manual`)
    #[serde(default)]
    manual: String,
}

#[derive(Deserialize)]
struct List {
    mods: Vec<Addition>,
}

static ADDITIONS: LazyLock<Vec<Entry>> = LazyLock::new(|| {
    let list: List = match serde_json::from_str(include_str!("../carried/hub-additions.json")) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("mods: hub-additions.json cannot be read: {e}");
            return Vec::new();
        }
    };
    let here = ffi::platform_tag();
    list.mods
        .into_iter()
        .filter(|a| a.platforms.as_ref().is_none_or(|p| p.iter().any(|t| t == here)))
        .map(|a| {
            let mut e = a.entry;
            if !a.manual.is_empty() {
                // the launcher never downloads a manual one, whatever the entry says
                e.download = None;
                e.manual = a.manual;
            }
            e
        })
        .collect()
});

/// Adds every addition whose id the hub's list does not have (after the hub's own mods, in the file's order).
pub fn add_to(catalog: &mut Catalog) {
    for a in ADDITIONS.iter() {
        if !catalog.mods.iter().any(|m| m.id == a.id) {
            catalog.mods.push(a.clone());
        }
    }
}

/// Whether `d` is exactly the download of one of the additions (address, file, size and SHA-256).
pub fn is_addition(d: &Download) -> bool {
    ADDITIONS.iter().filter_map(|a| a.download.as_ref()).any(|o| o == d)
}
