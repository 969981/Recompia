// SPDX-License-Identifier: GPL-3.0-or-later
//! The Mods page's Browse tab: the Modtopia mod hub's list, `catalog.json` (schema 1). Its mods are checked by hand by the hub's team
//! before they are listed. The launcher reads it from the hub only, keeps the last good copy in its settings folder for browsing offline,
//! and shows only the mods made for this game version whose `needs` range holds the core this launcher carries. Fields it does not know
//! are ignored (schema 1 may grow); an entry it cannot read is left out, never the whole list.

use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::Mutex;

use crate::download;
use crate::modtopia_ffi::{self as ffi, CORE_VERSION, GAME_VERSION};

pub const CATALOG_URL: &str = "https://pokopia-native.vercel.app/mods/catalog.json";
/// Where the hub's mod pages live (a mod's `page`, the hub, the submit form): the only links of the list the launcher opens.
pub const HUB_SITE: &str = "https://pokopia-native.vercel.app/";
/// The hub's download server: every install comes from here and nowhere else.
pub const DOWNLOAD_HOST: &str = "https://downloads.pokopia-native.workers.dev/";
const SCHEMA: u64 = 1;
const LIST_LIMIT: u64 = 4 << 20;

/// A published package: its address on the download server, file name, size and SHA-256.
#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
pub struct Download {
    pub url: String,
    pub file: String,
    pub size: u64,
    pub sha256: String,
}

/// One mod of the list as the hub publishes it (the fields the launcher uses).
#[derive(Deserialize, Clone, Debug)]
pub struct Entry {
    pub id: String,
    pub name: String,
    pub version: String,
    #[serde(default)]
    pub summary: String,
    /// "gameplay" or "visual"
    #[serde(default)]
    pub affects: String,
    #[serde(default)]
    pub network: bool,
    #[serde(default)]
    pub game: Vec<String>,
    #[serde(default)]
    pub needs: String,
    #[serde(default)]
    pub authors: Vec<String>,
    #[serde(default)]
    pub license: String,
    #[serde(default)]
    pub page: String,
    /// null until the mod is out ("Coming soon")
    #[serde(default)]
    pub download: Option<Download>,
}

/// The list, read.
#[derive(Clone, Debug, Default)]
pub struct Catalog {
    /// nothing is installable yet: every Install stays off
    pub preview: bool,
    pub site: String,
    pub submit: String,
    pub mods: Vec<Entry>,
}

/// Reads a catalog.json. Err for anything but schema 1; entries that cannot be read are left out.
pub fn parse(body: &[u8]) -> Result<Catalog, String> {
    let v: serde_json::Value = serde_json::from_slice(body).map_err(|e| format!("The mod list cannot be read: {}", e))?;
    if v.get("schema").and_then(|s| s.as_u64()) != Some(SCHEMA) {
        return Err("The mod list is in a newer format than this launcher reads. Update the launcher to browse mods.".into());
    }
    let text = |k: &str| v.get(k).and_then(|s| s.as_str()).unwrap_or("").to_string();
    let mods = v.get("mods").and_then(|m| m.as_array()).map(|a| a.iter().filter_map(|m| serde_json::from_value(m.clone()).ok()).collect()).unwrap_or_default();
    Ok(Catalog { preview: v.get("preview").and_then(|p| p.as_bool()).unwrap_or(false), site: text("site"), submit: text("submit"), mods })
}

/// Made for this game version, and for a core this launcher has.
pub fn fits(e: &Entry) -> bool {
    e.game.iter().any(|g| g == GAME_VERSION) && ffi::version_satisfies(CORE_VERSION, &e.needs) == Some(true)
}

/// A link of the list the launcher may open in the browser: a page of the hub. Anything else shows as text only.
pub fn hub_link(url: &str) -> bool {
    url.starts_with(HUB_SITE) && !url.chars().any(|c| c.is_whitespace() || c == '"' || c == '&' || c == '\\')
}

/// A download the launcher takes: on the hub's download server, over https, a plain `.pkmod` file name.
pub fn download_ok(d: &Download) -> bool {
    let name_ok = !d.file.is_empty() && d.file.ends_with(".pkmod") && !d.file.starts_with('.') && !d.file.contains(['/', '\\', ':', '\0']);
    let hex_ok = d.sha256.len() == 64 && d.sha256.chars().all(|c| c.is_ascii_hexdigit());
    d.url.starts_with(DOWNLOAD_HOST) && !d.url.contains(['@', '\\', ' ', '"']) && name_ok && hex_ok && d.size > 0
}

/// This session's copy of the list, and whether it came from the cache file (offline).
#[derive(Default)]
pub struct CatalogStore(Mutex<Option<(Catalog, bool)>>);

/// The list: this session's copy unless `refresh`, else one GET from the hub, else the cache file `cache` (offline), else an error.
/// The lock is held over the GET, so the page's calls make one request between them.
pub fn load(store: &CatalogStore, cache: &Path, refresh: bool) -> Result<(Catalog, bool), String> {
    let mut mem = store.0.lock().unwrap_or_else(|e| e.into_inner());
    if let (Some(c), false) = (mem.as_ref(), refresh) {
        return Ok(c.clone());
    }
    let fetched = fetch().and_then(|body| parse(&body).map(|c| (body, c)));
    let got = match fetched {
        Ok((body, c)) => {
            if let Err(e) = crate::settings::write_atomic(cache, &String::from_utf8_lossy(&body)) {
                eprintln!("mod list not cached: {}", e);
            }
            (c, false)
        }
        Err(e) => {
            eprintln!("mod list: {}", e);
            let cached = std::fs::read(cache).ok().and_then(|b| parse(&b).ok());
            match cached.or_else(|| mem.take().map(|(c, _)| c)) {
                Some(c) => (c, true),
                None => return Err("The mod list could not be loaded. Check the internet connection and try again.".into()),
            }
        }
    };
    *mem = Some(got.clone());
    Ok(got)
}

fn fetch() -> Result<Vec<u8>, String> {
    use std::io::Read;
    let resp = download::agent(false).get(CATALOG_URL).call().map_err(|e| e.to_string())?;
    let mut body = Vec::new();
    resp.into_reader().take(LIST_LIMIT).read_to_end(&mut body).map_err(|e| e.to_string())?;
    Ok(body)
}

/// One mod of the Browse tab.
#[derive(Serialize, Clone, Debug)]
pub struct CardView {
    pub id: String,
    pub name: String,
    pub version: String,
    pub summary: String,
    pub authors: Vec<String>,
    pub license: String,
    pub gameplay: bool,
    pub network: bool,
    /// the mod's page on the hub, "" when it is not one the launcher opens
    pub page: String,
    /// it has a download (else "Coming soon")
    pub available: bool,
    /// the installed version, "" when it is not installed
    pub installed: String,
    /// the list has a newer version than the installed one
    pub newer: bool,
}

/// The Browse tab.
#[derive(Serialize, Clone, Debug)]
pub struct CatalogView {
    /// read from the copy kept for offline browsing
    pub offline: bool,
    pub preview: bool,
    /// the hub's mods page and its submit form, "" when not links the launcher opens
    pub hub: String,
    pub submit: String,
    pub mods: Vec<CardView>,
}

/// The cards of the mods that fit, with what is installed (`installed`: id and version of each installed mod).
pub fn view(c: &Catalog, offline: bool, installed: &[(String, String)]) -> CatalogView {
    let link = |u: &str| if hub_link(u) { u.to_string() } else { String::new() };
    let mods = c
        .mods
        .iter()
        .filter(|e| fits(e))
        .map(|e| {
            let have = installed.iter().find(|(id, _)| *id == e.id).map(|(_, v)| v.clone()).unwrap_or_default();
            CardView {
                id: e.id.clone(),
                name: e.name.clone(),
                version: e.version.clone(),
                summary: e.summary.clone(),
                authors: e.authors.clone(),
                license: e.license.clone(),
                gameplay: e.affects != "visual",
                network: e.network,
                page: link(&e.page),
                available: e.download.as_ref().is_some_and(download_ok),
                newer: !have.is_empty() && ffi::version_compare(&e.version, &have) == std::cmp::Ordering::Greater,
                installed: have,
            }
        })
        .collect();
    let hub = if c.site.is_empty() { String::new() } else { link(&format!("{}/mods", c.site.trim_end_matches('/'))) };
    CatalogView { offline, preview: c.preview, hub, submit: link(&c.submit), mods }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"{"schema": 1, "preview": false, "site": "https://pokopia-native.vercel.app", "submit": "https://pokopia-native.vercel.app/mods/submit",
      "future_field": [1, 2],
      "mods": [
        {"id": "auto_run", "name": "Auto-Run", "version": "1.0.0", "affects": "visual", "network": false, "game": ["2.0.1", "1.0.5"],
         "needs": ">=0.3.0 <1.0.0", "authors": ["The Modtopia contributors"], "license": "MIT", "page": "https://pokopia-native.vercel.app/mods/auto-run",
         "download": {"url": "https://downloads.pokopia-native.workers.dev/mods/auto-run/1.0.0/auto_run-1.0.0.pkmod", "file": "auto_run-1.0.0.pkmod",
                      "size": 77330, "sha256": "e70a6ef937952a9fe23aa7847df56f787fa8164129d53a081c1bec664b228ab8"}, "new": true},
        {"id": "old_only", "name": "Old", "version": "1.0.0", "game": ["1.0.5"], "needs": ">=0.1.0"},
        {"id": "too_new", "name": "New", "version": "1.0.0", "game": ["2.0.1"], "needs": ">=0.7.0"},
        {"id": "soon", "name": "Soon", "version": "0.9.0", "affects": "gameplay", "game": ["2.0.1"], "needs": ">=0.1.0 <1.0.0", "download": null},
        {"name": "no id"}
      ]}"#;

    #[test]
    fn reads_schema_1_and_keeps_only_what_fits() {
        let c = parse(SAMPLE.as_bytes()).unwrap();
        assert_eq!(c.mods.len(), 4, "the entry without an id is left out, not the list");
        let v = view(&c, false, &[("auto_run".into(), "0.9.0".into())]);
        let ids: Vec<&str> = v.mods.iter().map(|m| m.id.as_str()).collect();
        assert_eq!(ids, ["auto_run", "soon"]);
        assert!(v.mods[0].available && v.mods[0].newer && !v.mods[0].gameplay && v.mods[0].installed == "0.9.0");
        assert!(!v.mods[1].available && v.mods[1].gameplay && v.mods[1].installed.is_empty());
        assert_eq!(v.submit, "https://pokopia-native.vercel.app/mods/submit");
        assert_eq!(v.hub, "https://pokopia-native.vercel.app/mods");
        assert!(parse(br#"{"schema": 2, "mods": []}"#).is_err());
    }

    #[test]
    fn downloads_come_from_the_hub_server_only() {
        let good = Download { url: format!("{DOWNLOAD_HOST}mods/a/1.0.0/a-1.0.0.pkmod"), file: "a-1.0.0.pkmod".into(), size: 10, sha256: "a".repeat(64) };
        assert!(download_ok(&good));
        let bad = |f: &dyn Fn(&mut Download)| {
            let mut d = good.clone();
            f(&mut d);
            !download_ok(&d)
        };
        assert!(bad(&|d| d.url = "https://downloads.pokopia-native.workers.dev.evil.example/a.pkmod".into()));
        assert!(bad(&|d| d.url = "http://downloads.pokopia-native.workers.dev/a.pkmod".into()));
        assert!(bad(&|d| d.url = "https://github.com/x/a.pkmod".into()));
        assert!(bad(&|d| d.file = "../a.pkmod".into()));
        assert!(bad(&|d| d.file = "a.dll".into()));
        assert!(bad(&|d| d.sha256 = "zz".into()));
        assert!(bad(&|d| d.size = 0));
        assert!(hub_link("https://pokopia-native.vercel.app/mods/auto-run") && !hub_link("https://pokopia-native.vercel.app.evil.example/x"));
    }
}
