// SPDX-License-Identifier: GPL-3.0-or-later
//! The player's own themes. A theme pack is a folder in `<launcher data>/themes/` with a `theme.json` and the pictures it names; the
//! player adds one as a folder or a .zip (`add`), and the launcher checks it before it is kept. A theme changes the look of the window
//! (colours, background, logo, rail marks; the page side is src/themes.js) and, through branding.rs, the icon of this window and of
//! the exes and the Steam shortcut pictures (`load_art`). The launcher ships no theme: every picture comes from the player's PC.
//!
//! A pack is checked as untrusted: plain file names, real files only (no links), PNG / WebP / JPEG by their first bytes (no SVG),
//! a size cap on every file and on the pack, colours only as `#hex` or `rgb()` / `rgba()` numbers so no text of a pack reaches the
//! style sheet as anything else. A pack that fails is listed with the reason and never applied.
//!
//! theme.json (format 1): { "format": 1, "name", "themes": [ { "id", "name", "background", "background_position",
//! "colors": { "<token>": "<colour>" }, "logo", "mark", "icon", "steam": { "wide", "portrait", "hero", "logo", "icon" } } ] }.
//! Only format, name and each theme's id and name are required; what a theme leaves out stays the launcher's own.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::zipin::{self, Limits};

/// The protocol the window loads a theme's pictures through (http://theme-data.localhost/ on Windows, theme-data://localhost/ elsewhere).
pub const PROTOCOL: &str = "theme-data";
const MANIFEST: &str = "theme.json";
const LEDGER: &str = ".steam-art.json";
const MAX_MANIFEST: u64 = 256 * 1024;
const MAX_PICTURE: u64 = 16_000_000;
const LIMITS: Limits = Limits { max_entries: 200, max_file: MAX_PICTURE, max_total: 64_000_000 };
const MAX_ZIP: u64 = 80_000_000;
const MAX_THEMES: usize = 16;
const LEDGER_KEEP: usize = 400;

static DIR: OnceLock<PathBuf> = OnceLock::new();

/// Sets the themes folder (once, at start).
pub fn init(dir: PathBuf) {
    let _ = DIR.set(dir);
}

pub fn dir() -> Option<&'static Path> {
    DIR.get().map(PathBuf::as_path)
}

/// The themes folder, made when it is missing.
pub fn ensure_dir() -> Result<&'static Path, String> {
    let dir = dir().ok_or("The themes folder is not known yet.")?;
    fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    Ok(dir)
}

// ---- the manifest ----------------------------------------------------------------------------------------------------------------

#[derive(Deserialize, Clone, Default, Debug)]
#[serde(default)]
struct SteamDef {
    wide: Option<String>,
    portrait: Option<String>,
    hero: Option<String>,
    logo: Option<String>,
    icon: Option<String>,
}

#[derive(Deserialize, Clone, Default, Debug)]
#[serde(default)]
struct ThemeDef {
    id: String,
    name: String,
    background: Option<String>,
    background_position: Option<String>,
    colors: BTreeMap<String, String>,
    logo: Option<String>,
    mark: Option<String>,
    icon: Option<String>,
    steam: SteamDef,
}

#[derive(Deserialize, Debug, Default)]
#[serde(default)]
struct Manifest {
    format: u32,
    name: String,
    themes: Vec<ThemeDef>,
}

/// What the page needs of one theme. Picture paths are `<pack folder>/<file>`, served by the `theme-data` protocol.
#[derive(Serialize, Clone, Debug)]
pub struct ThemeInfo {
    pub key: String,
    pub name: String,
    pub background: Option<String>,
    pub background_position: Option<String>,
    pub colors: BTreeMap<String, String>,
    pub logo: Option<String>,
    pub mark: Option<String>,
}

#[derive(Serialize, Clone, Debug)]
pub struct PackInfo {
    pub folder: String,
    pub name: String,
    /// Changes whenever the pack's files do, so the window asks for its pictures again.
    pub version: u64,
    pub themes: Vec<ThemeInfo>,
    /// Why the pack cannot be used; its themes are empty then.
    pub error: Option<String>,
}

/// A pack that was just added.
#[derive(Serialize, Clone, Debug)]
pub struct Added {
    pub name: String,
    pub themes: usize,
}

#[derive(Serialize, Default, Clone, Debug)]
pub struct ThemeList {
    pub packs: Vec<PackInfo>,
}

// ---- checking a pack -------------------------------------------------------------------------------------------------------------

fn slug(s: &str) -> String {
    let mut out = String::new();
    for c in s.to_lowercase().chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c);
        } else if !out.ends_with('-') {
            out.push('-');
        }
    }
    let out: String = out.trim_matches('-').chars().take(40).collect();
    let out = out.trim_matches('-').to_string();
    if out.is_empty() { "pack".into() } else { out }
}

fn id_ok(s: &str) -> bool {
    !s.is_empty() && s.len() <= 40 && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

/// A saved theme: empty, or `<pack folder>/<theme id>`.
pub fn key_ok(key: &str) -> bool {
    key.is_empty() || key.split_once('/').is_some_and(|(pack, id)| id_ok(pack) && id_ok(id))
}

fn text_ok(s: &str, max: usize) -> bool {
    let t = s.trim();
    !t.is_empty() && t.chars().count() <= max && !t.chars().any(char::is_control)
}

/// A colour a pack may name: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, or `rgb(...)` / `rgba(...)` with plain numbers. Nothing else
/// (no `url(`, no `var(`, no `;` or `}`): the page writes these into a style sheet.
pub fn valid_color(s: &str) -> bool {
    if let Some(h) = s.strip_prefix('#') {
        return matches!(h.len(), 3 | 4 | 6 | 8) && h.bytes().all(|b| b.is_ascii_hexdigit());
    }
    let inner = s.strip_prefix("rgba(").or_else(|| s.strip_prefix("rgb(")).and_then(|r| r.strip_suffix(')'));
    inner.is_some_and(|i| !i.is_empty() && i.len() <= 40 && i.bytes().all(|b| b.is_ascii_digit() || matches!(b, b',' | b'.' | b' ' | b'%')))
}

fn position_ok(s: &str) -> bool {
    !s.is_empty() && s.len() <= 20 && s.bytes().all(|b| b.is_ascii_digit() || matches!(b, b'.' | b'%' | b' '))
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    /// PNG, WebP or JPEG
    Picture,
    Png,
}

fn is_png(b: &[u8]) -> bool {
    b.starts_with(b"\x89PNG\r\n\x1a\n")
}

/// Checks one file a theme names: a plain name, a real file under the size cap, of the kind its first bytes say. Returns its path and
/// those first bytes.
fn open_file(root: &Path, rel: &str, kind: Kind, field: &str) -> Result<(PathBuf, Vec<u8>), String> {
    let ok_name = zipin::entry_name_ok(rel) && rel.len() <= 100 && rel.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-' | b' ' | b'/')) && !rel.split('/').any(|p| p.starts_with('.'));
    if !ok_name {
        return Err(format!("{field}: \"{rel}\" is not a plain file name (letters, digits, . _ - and spaces; folders with /)."));
    }
    let path = root.join(rel);
    let meta = fs::symlink_metadata(&path).map_err(|_| format!("{field}: the file {rel} is not in the pack."))?;
    if !meta.is_file() {
        return Err(format!("{field}: {rel} is not a plain file."));
    }
    if meta.len() > MAX_PICTURE {
        return Err(format!("{field}: {rel} is larger than {} MB.", MAX_PICTURE / 1_000_000));
    }
    let mut head = Vec::new();
    fs::File::open(&path).and_then(|f| f.take(32).read_to_end(&mut head)).map_err(|e| format!("{field}: cannot read {rel}: {e}"))?;
    let good = match kind {
        Kind::Picture => is_png(&head) || (head.len() > 12 && &head[..4] == b"RIFF" && &head[8..12] == b"WEBP") || head.starts_with(b"\xFF\xD8\xFF"),
        Kind::Png => is_png(&head),
    };
    if !good {
        let want = if kind == Kind::Png { "a PNG" } else { "a PNG, WebP or JPEG" };
        return Err(format!("{field}: {rel} is not {want} picture."));
    }
    Ok((path, head))
}

/// The bytes of a checked file.
fn read_file(root: &Path, rel: &str, kind: Kind, field: &str) -> Result<Vec<u8>, String> {
    let (path, _) = open_file(root, rel, kind, field)?;
    fs::read(&path).map_err(|e| format!("{field}: cannot read {rel}: {e}"))
}

/// The width and height of a PNG (its IHDR), or None.
fn png_size(b: &[u8]) -> Option<(u32, u32)> {
    if !is_png(b) || b.len() < 24 || &b[12..16] != b"IHDR" {
        return None;
    }
    Some((u32::from_be_bytes(b[16..20].try_into().ok()?), u32::from_be_bytes(b[20..24].try_into().ok()?)))
}

/// The side of a PNG icon the launcher can use as an .ico: square, 1 to 256 pixels. `png` may be just the start of the file.
fn icon_side(png: &[u8]) -> Result<u32, String> {
    let (w, h) = png_size(png).ok_or("The icon is not a PNG picture.")?;
    if w != h || w == 0 || w > 256 {
        return Err(format!("The icon must be square and at most 256 x 256 pixels (it is {w} x {h})."));
    }
    Ok(w)
}

/// An .ico holding one PNG (up to 256 x 256, square): what an exe takes as its icon.
pub fn png_to_ico(png: &[u8]) -> Result<Vec<u8>, String> {
    let w = icon_side(png)?;
    let side = if w == 256 { 0 } else { w as u8 };
    let mut out = vec![0, 0, 1, 0, 1, 0, side, side, 0, 0, 1, 0, 32, 0];
    out.extend_from_slice(&(png.len() as u32).to_le_bytes());
    out.extend_from_slice(&22u32.to_le_bytes());
    out.extend_from_slice(png);
    Ok(out)
}

/// Reads and checks theme.json and every file it names. Err: the first problem, in words for the player.
fn check_pack(root: &Path) -> Result<Manifest, String> {
    let path = root.join(MANIFEST);
    let meta = fs::symlink_metadata(&path).map_err(|_| format!("There is no {MANIFEST} in the pack."))?;
    if !meta.is_file() || meta.len() > MAX_MANIFEST {
        return Err(format!("{MANIFEST} is not a plain file under {} KB.", MAX_MANIFEST / 1024));
    }
    let text = fs::read_to_string(&path).map_err(|e| format!("{MANIFEST} cannot be read: {e}"))?;
    let m: Manifest = serde_json::from_str(text.trim_start_matches('\u{feff}')).map_err(|e| format!("{MANIFEST}: {e}"))?;
    if m.format != 1 {
        return Err(format!("{MANIFEST}: \"format\" must be 1 (it is {}).", m.format));
    }
    if !text_ok(&m.name, 60) {
        return Err(format!("{MANIFEST}: \"name\" is needed (up to 60 characters)."));
    }
    if m.themes.is_empty() || m.themes.len() > MAX_THEMES {
        return Err(format!("{MANIFEST}: a pack holds 1 to {MAX_THEMES} themes."));
    }
    let mut seen = Vec::new();
    for t in &m.themes {
        let who = format!("theme \"{}\"", t.id);
        if !id_ok(&t.id) {
            return Err(format!("{MANIFEST}: a theme id must be lowercase letters, digits and - (got \"{}\").", t.id));
        }
        if seen.contains(&t.id) {
            return Err(format!("{MANIFEST}: the theme id \"{}\" is used twice.", t.id));
        }
        seen.push(t.id.clone());
        if !text_ok(&t.name, 40) {
            return Err(format!("{MANIFEST}: {who} needs a name of up to 40 characters."));
        }
        for (k, v) in &t.colors {
            if !id_ok(k) {
                return Err(format!("{MANIFEST}: {who}: \"{k}\" is not a colour name."));
            }
            if !valid_color(v) {
                return Err(format!("{MANIFEST}: {who}: colour {k} must be like #1a2b3c or rgba(10, 20, 30, 0.5)."));
            }
        }
        if t.background_position.as_deref().is_some_and(|p| !position_ok(p)) {
            return Err(format!("{MANIFEST}: {who}: background_position must be like 50% 56%."));
        }
        for (field, file, kind) in files_of(t) {
            let head = open_file(root, file, kind, &format!("{who} {field}"))?.1;
            // the icon becomes an .ico: square and small (its size is in the first bytes of the PNG)
            if field == "icon" {
                icon_side(&head).map_err(|e| format!("{who} icon: {e}"))?;
            }
        }
    }
    Ok(m)
}

/// Every file a theme names: (field, file, what it must be).
fn files_of(t: &ThemeDef) -> Vec<(&'static str, &String, Kind)> {
    let s = &t.steam;
    [
        ("background", &t.background, Kind::Picture),
        ("logo", &t.logo, Kind::Picture),
        ("mark", &t.mark, Kind::Picture),
        ("icon", &t.icon, Kind::Png),
        ("steam wide", &s.wide, Kind::Png),
        ("steam portrait", &s.portrait, Kind::Png),
        ("steam hero", &s.hero, Kind::Png),
        ("steam logo", &s.logo, Kind::Png),
        ("steam icon", &s.icon, Kind::Png),
    ]
    .into_iter()
    .filter_map(|(f, v, k)| v.as_ref().map(|v| (f, v, k)))
    .collect()
}

// ---- listing, adding, removing -----------------------------------------------------------------------------------------------------

fn folder_version(root: &Path) -> u64 {
    fn walk(dir: &Path, depth: u32, newest: &mut u64) {
        let Ok(rd) = fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            if let Ok(m) = e.metadata() {
                let t = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_millis() as u64);
                *newest = (*newest).max(t).max(m.len());
                if m.is_dir() && depth < 4 {
                    walk(&e.path(), depth + 1, newest);
                }
            }
        }
    }
    let mut newest = 0;
    walk(root, 0, &mut newest);
    newest
}

fn info_of(folder: &str, t: &ThemeDef) -> ThemeInfo {
    let rel = |f: &Option<String>| f.as_ref().map(|f| format!("{folder}/{f}"));
    ThemeInfo {
        key: format!("{folder}/{}", t.id),
        name: t.name.trim().to_string(),
        background: rel(&t.background),
        background_position: t.background_position.clone(),
        colors: t.colors.clone(),
        logo: rel(&t.logo),
        mark: rel(&t.mark),
    }
}

fn pack_info(root: &Path, folder: &str) -> PackInfo {
    match check_pack(root) {
        Ok(m) => PackInfo {
            folder: folder.into(),
            name: m.name.trim().into(),
            version: folder_version(root),
            themes: m.themes.iter().map(|t| info_of(folder, t)).collect(),
            error: None,
        },
        Err(e) => PackInfo { folder: folder.into(), name: folder.into(), version: 0, themes: Vec::new(), error: Some(e) },
    }
}

/// Every pack in the themes folder, the broken ones with their reason.
pub fn list() -> ThemeList {
    let Some(dir) = dir() else { return ThemeList::default() };
    let mut packs: Vec<PackInfo> = fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|n| id_ok(n))
        .map(|n| pack_info(&dir.join(&n), &n))
        .collect();
    packs.sort_by_key(|p| p.name.to_lowercase());
    ThemeList { packs }
}

/// Copies a pack folder the player chose: real files and folders only (a link stops it), under the size and count caps.
fn copy_pack(src: &Path, dst: &Path, depth: u32, count: &mut usize, total: &mut u64) -> Result<(), String> {
    if depth > 4 {
        return Err("The folder is nested too deeply.".into());
    }
    fs::create_dir_all(dst).map_err(|e| format!("cannot create {}: {e}", dst.display()))?;
    for e in fs::read_dir(src).map_err(|e| format!("cannot read {}: {e}", src.display()))?.flatten() {
        let meta = fs::symlink_metadata(e.path()).map_err(|e| e.to_string())?;
        let name = e.file_name();
        if meta.file_type().is_symlink() {
            return Err(format!("{} is a link; links are not allowed in a theme.", e.path().display()));
        }
        if meta.is_dir() {
            copy_pack(&e.path(), &dst.join(&name), depth + 1, count, total)?;
            continue;
        }
        *count += 1;
        *total += meta.len();
        if *count > LIMITS.max_entries || *total > LIMITS.max_total || meta.len() > LIMITS.max_file {
            return Err(format!("The folder is too large for a theme (at most {} files and {} MB).", LIMITS.max_entries, LIMITS.max_total / 1_000_000));
        }
        fs::copy(e.path(), dst.join(&name)).map_err(|e| format!("cannot copy {}: {e}", name.to_string_lossy()))?;
    }
    Ok(())
}

/// The folder inside `dir` that holds theme.json: `dir` itself, or its one subfolder (a .zip made from a folder has one).
fn pack_root(dir: &Path) -> Result<PathBuf, String> {
    if dir.join(MANIFEST).is_file() {
        return Ok(dir.to_path_buf());
    }
    let subs: Vec<PathBuf> = fs::read_dir(dir).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).filter(|p| p.is_dir() && p.join(MANIFEST).is_file()).collect();
    match subs.as_slice() {
        [one] => Ok(one.clone()),
        _ => Err(format!("There is no {MANIFEST} in what you chose.")),
    }
}

/// Adds a pack from a folder or a .zip: unpacked or copied beside the others, checked, then put in place (a pack of the same name is
/// replaced). Nothing is kept when it fails.
pub fn add(src: &Path) -> Result<Added, String> {
    let dir = ensure_dir()?;
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos());
    let incoming = dir.join(format!(".incoming-{}-{stamp}", std::process::id()));
    let result = add_from(src, dir, &incoming);
    let _ = fs::remove_dir_all(&incoming);
    result
}

fn add_from(src: &Path, dir: &Path, incoming: &Path) -> Result<Added, String> {
    fs::create_dir_all(incoming).map_err(|e| format!("cannot create {}: {e}", incoming.display()))?;
    let meta = fs::metadata(src).map_err(|e| format!("cannot read {}: {e}", src.display()))?;   // what the player picked may be a link; what is inside it may not
    if meta.is_dir() {
        copy_pack(src, incoming, 0, &mut 0, &mut 0)?;
    } else if meta.is_file() {
        if meta.len() > MAX_ZIP {
            return Err(format!("That file is larger than {} MB.", MAX_ZIP / 1_000_000));
        }
        let bytes = fs::read(src).map_err(|e| format!("cannot read {}: {e}", src.display()))?;
        zipin::extract(&bytes, incoming, &LIMITS)?;
    } else {
        return Err("Choose a folder or a .zip file.".into());
    }
    let root = pack_root(incoming)?;
    let m = check_pack(&root)?;
    let folder = slug(&m.name);
    let dest = dir.join(&folder);
    let old = dir.join(format!(".old-{folder}"));
    let _ = fs::remove_dir_all(&old);
    let had = dest.exists();
    if had {
        fs::rename(&dest, &old).map_err(|e| format!("cannot replace the pack {folder}: {e}"))?;
    }
    if let Err(e) = fs::rename(&root, &dest) {
        if had {
            let _ = fs::rename(&old, &dest);
        }
        return Err(format!("cannot add the pack: {e}"));
    }
    let _ = fs::remove_dir_all(&old);
    Ok(Added { name: m.name.trim().to_string(), themes: m.themes.len() })
}

/// Removes a pack folder (by its folder name). Its pictures stay in the Steam ledger, so Steam art written from it is still ours to replace.
pub fn remove(folder: &str) -> Result<(), String> {
    if !id_ok(folder) {
        return Err("That is not a theme pack.".into());
    }
    let path = dir().ok_or("The themes folder is not known yet.")?.join(folder);
    if !path.is_dir() {
        return Err("That theme pack is already gone.".into());
    }
    fs::remove_dir_all(&path).map_err(|e| format!("cannot remove {}: {e}", path.display()))
}

// ---- the art the system shows ------------------------------------------------------------------------------------------------------

/// The pictures of one theme that branding.rs puts outside the window: the window icon (PNG), the exe icon (.ico, the PNG wrapped) and
/// the Steam pictures by kind (wide, portrait, hero, logo, icon). A kind the theme does not name is absent: the launcher's own is used.
/// The Steam icon falls back to the theme's `icon`. One icon serves both channels.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct Art {
    pub window_png: Option<Vec<u8>>,
    pub ico: Option<Vec<u8>>,
    pub steam: HashMap<&'static str, Vec<u8>>,
}

/// The last art loaded, with the pack's version: Branding::of asks on every Steam status, Play and start, and a pack is read once.
static ART: Mutex<Option<(String, u64, Option<Arc<Art>>)>> = Mutex::new(None);

/// The art of theme `key` (`<pack folder>/<id>`), None when the key names nothing usable or the theme has no such pictures.
pub fn load_art(key: &str) -> Option<Arc<Art>> {
    let root = dir()?;
    let version = folder_version(&root.join(key.split_once('/')?.0));
    let mut last = ART.lock().ok()?;
    if let Some((k, v, art)) = last.as_ref() {
        if k == key && *v == version {
            return art.clone();
        }
    }
    let art = load_art_in(root, key).map(Arc::new);
    *last = Some((key.to_string(), version, art.clone()));
    art
}

fn load_art_in(root: &Path, key: &str) -> Option<Art> {
    let (folder, id) = key.split_once('/')?;
    if !id_ok(folder) || !id_ok(id) {
        return None;
    }
    let pack = root.join(folder);
    let m = check_pack(&pack).ok()?;
    let t = m.themes.iter().find(|t| t.id == id)?;
    let get = |f: &Option<String>| f.as_ref().and_then(|f| read_file(&pack, f, Kind::Png, "art").ok());
    let icon = get(&t.icon);
    let ico = icon.as_deref().and_then(|p| png_to_ico(p).ok());
    let mut steam = HashMap::new();
    for (kind, file) in [("wide", &t.steam.wide), ("portrait", &t.steam.portrait), ("hero", &t.steam.hero), ("logo", &t.steam.logo)] {
        if let Some(b) = get(file) {
            steam.insert(kind, b);
        }
    }
    if let Some(b) = get(&t.steam.icon).or_else(|| icon.clone()) {
        steam.insert("icon", b);
    }
    let art = Art { window_png: icon, ico, steam };
    (art != Art::default()).then_some(art)
}

// ---- the Steam ledger --------------------------------------------------------------------------------------------------------------
// Every theme picture written into a Steam grid folder is noted here by its hash, so a later launcher still knows it wrote it (and may
// replace or delete it), even after the theme is changed or removed. A picture the player chose themselves is never in the ledger.

fn ledger_key(kind: &str, bytes: &[u8]) -> String {
    format!("{kind}:{:x}", Sha256::digest(bytes))
}

fn ledger_read(root: &Path) -> Vec<String> {
    fs::read(root.join(LEDGER)).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

fn ledger_add_in(root: &Path, kind: &str, bytes: &[u8]) -> Result<(), String> {
    let key = ledger_key(kind, bytes);
    let mut all = ledger_read(root);
    if all.contains(&key) {
        return Ok(());
    }
    all.push(key);
    if all.len() > LEDGER_KEEP {
        all.drain(..all.len() - LEDGER_KEEP);
    }
    fs::create_dir_all(root).map_err(|e| e.to_string())?;
    crate::settings::write_atomic(&root.join(LEDGER), &serde_json::to_string(&all).map_err(|e| e.to_string())?)
}

/// Notes that `bytes` is about to be written as Steam art of `kind`.
pub fn ledger_add(kind: &str, bytes: &[u8]) -> Result<(), String> {
    match dir() {
        Some(root) => ledger_add_in(root, kind, bytes),
        None => Ok(()),
    }
}

/// Whether `bytes` is a picture of `kind` that a theme wrote into Steam before.
pub fn ledger_has(kind: &str, bytes: &[u8]) -> bool {
    dir().is_some_and(|root| ledger_read(root).contains(&ledger_key(kind, bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::zipout::ZipOut;

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-themes-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    /// A 1 x 1 PNG header is enough for the checks (magic and IHDR); `side` makes the icon size.
    fn png(side: u32) -> Vec<u8> {
        let mut b = b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR".to_vec();
        b.extend_from_slice(&side.to_be_bytes());
        b.extend_from_slice(&side.to_be_bytes());
        b.extend_from_slice(&[8, 6, 0, 0, 0, 0, 0, 0, 0]);
        b
    }

    fn webp() -> Vec<u8> {
        let mut b = b"RIFF\x10\0\0\0WEBPVP8 ".to_vec();
        b.extend_from_slice(&[0; 8]);
        b
    }

    fn pack(root: &Path, manifest: &str, files: &[(&str, Vec<u8>)]) {
        fs::create_dir_all(root).unwrap();
        fs::write(root.join(MANIFEST), manifest).unwrap();
        for (n, d) in files {
            fs::write(root.join(n), d).unwrap();
        }
    }

    const GOOD: &str = r##"{"format":1,"name":"Camp Pack","themes":[{"id":"camp","name":"Camp","background":"bg.webp","colors":{"accent":"#ffc23d","glass-tint":"rgba(36, 18, 6, 0.44)"},"icon":"icon.png","steam":{"wide":"wide.png"}}]}"##;

    fn good_files() -> Vec<(&'static str, Vec<u8>)> {
        vec![("bg.webp", webp()), ("icon.png", png(64)), ("wide.png", png(8))]
    }

    #[test]
    fn colours_are_numbers_only() {
        for ok in ["#fff", "#ffcc00", "#ffcc0080", "rgb(1, 2, 3)", "rgba(10,20,30,0.5)", "rgba(10, 20, 30, 50%)"] {
            assert!(valid_color(ok), "{ok}");
        }
        for bad in ["url(x)", "red", "#ggg", "#12345", "rgb(1,2,3);}", "rgba(var(--a))", "rgb(1,2,3) x", "#fff}", "", "rgba()", "expression(1)"] {
            assert!(!valid_color(bad), "{bad}");
        }
    }

    #[test]
    fn a_good_pack_is_listed_with_its_theme_and_a_bad_one_with_a_reason() {
        let root = tmp("list");
        pack(&root.join("camp-pack"), GOOD, &good_files());
        pack(&root.join("broken"), r#"{"format":1,"name":"B","themes":[{"id":"x","name":"X","background":"missing.png"}]}"#, &[]);
        let packs: Vec<PackInfo> = ["camp-pack", "broken"].iter().map(|f| pack_info(&root.join(f), f)).collect();
        assert_eq!(packs[0].error, None);
        assert_eq!(packs[0].themes[0].key, "camp-pack/camp");
        assert_eq!(packs[0].themes[0].background.as_deref(), Some("camp-pack/bg.webp"));
        assert!(packs[1].error.as_ref().unwrap().contains("missing.png"));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn bad_manifests_say_why() {
        let root = tmp("bad");
        let cases: [(&str, &str); 7] = [
            (r#"{"format":2,"name":"A","themes":[{"id":"a","name":"A"}]}"#, "format"),
            (r#"{"format":1,"name":"A","themes":[]}"#, "1 to"),
            (r#"{"format":1,"name":"A","themes":[{"id":"A B","name":"A"}]}"#, "theme id"),
            (r#"{"format":1,"name":"A","themes":[{"id":"a","name":"A"},{"id":"a","name":"B"}]}"#, "twice"),
            (r#"{"format":1,"name":"A","themes":[{"id":"a","name":"A","colors":{"accent":"url(http://x)"}}]}"#, "colour accent"),
            (r#"{"format":1,"name":"A","themes":[{"id":"a","name":"A","background":"../x.png"}]}"#, "plain file name"),
            ("not json", "theme.json"),
        ];
        for (m, want) in cases {
            pack(&root.join("p"), m, &[]);
            let e = check_pack(&root.join("p")).err().unwrap_or_else(|| panic!("accepted {m}"));
            assert!(e.contains(want), "{m}: {e}");
        }
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_file_must_be_the_kind_it_is_named_for() {
        let root = tmp("magic");
        let m = r#"{"format":1,"name":"A","themes":[{"id":"a","name":"A","background":"bg.png","steam":{"wide":"w.webp"}}]}"#;
        pack(&root.join("p"), m, &[("bg.png", b"<svg xmlns='http://www.w3.org/2000/svg'/>".to_vec()), ("w.webp", webp())]);
        assert!(check_pack(&root.join("p")).unwrap_err().contains("not a PNG, WebP or JPEG"));
        pack(&root.join("p"), m, &[("bg.png", png(4)), ("w.webp", webp())]);
        assert!(check_pack(&root.join("p")).unwrap_err().contains("steam wide"), "Steam pictures must be PNG");
        pack(&root.join("p"), m, &[("bg.png", png(4)), ("w.webp", png(4))]);
        assert!(check_pack(&root.join("p")).is_ok());
        let _ = fs::remove_dir_all(&root);
    }

    #[cfg(unix)]
    #[test]
    fn a_link_is_not_a_file() {
        let root = tmp("link");
        pack(&root.join("p"), r#"{"format":1,"name":"A","themes":[{"id":"a","name":"A","background":"bg.png"}]}"#, &[]);
        fs::write(root.join("real.png"), png(4)).unwrap();
        std::os::unix::fs::symlink(root.join("real.png"), root.join("p/bg.png")).unwrap();
        assert!(check_pack(&root.join("p")).unwrap_err().contains("not a plain file"));
        let src = root.join("src");
        pack(&src, GOOD, &good_files());
        std::os::unix::fs::symlink("/etc/hostname", src.join("leak.txt")).unwrap();
        assert!(copy_pack(&src, &root.join("dst"), 0, &mut 0, &mut 0).unwrap_err().contains("link"));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_png_becomes_an_ico_the_exe_code_reads() {
        let ico = png_to_ico(&png(64)).unwrap();
        let images = crate::exe_icon::parse_ico(&ico).unwrap();
        assert_eq!(images.len(), 1);
        assert_eq!(images[0].data, png(64).as_slice());
        let big = png_to_ico(&png(256)).unwrap();
        assert_eq!(big[6], 0, "256 is stored as 0");
        assert!(png_to_ico(&png(300)).unwrap_err().contains("256"));
        let mut wide = png(8);
        wide[20..24].copy_from_slice(&16u32.to_be_bytes());
        assert!(png_to_ico(&wide).unwrap_err().contains("square"));
        assert!(png_to_ico(b"nope").is_err());
    }

    #[test]
    fn a_theme_with_art_gives_it_and_one_without_gives_none() {
        let root = tmp("art");
        pack(&root.join("camp-pack"), GOOD, &good_files());
        let art = load_art_in(&root, "camp-pack/camp").unwrap();
        assert_eq!(art.window_png.as_deref(), Some(png(64).as_slice()));
        assert!(crate::exe_icon::parse_ico(art.ico.as_deref().unwrap()).is_some());
        assert_eq!(art.steam["wide"], png(8));
        assert_eq!(art.steam["icon"], png(64), "the Steam icon falls back to the theme's icon");
        assert!(!art.steam.contains_key("hero"));
        pack(&root.join("plain"), r##"{"format":1,"name":"Plain","themes":[{"id":"p","name":"P","colors":{"accent":"#fff"}}]}"##, &[]);
        assert_eq!(load_art_in(&root, "plain/p"), None);
        assert_eq!(load_art_in(&root, "camp-pack/nope"), None);
        assert_eq!(load_art_in(&root, "../etc/x"), None);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn the_ledger_knows_what_a_theme_wrote_and_nothing_else() {
        let root = tmp("ledger");
        assert!(!ledger_read(&root).contains(&ledger_key("wide", b"x")));
        ledger_add_in(&root, "wide", b"theme picture").unwrap();
        ledger_add_in(&root, "wide", b"theme picture").unwrap();
        let all = ledger_read(&root);
        assert_eq!(all.len(), 1);
        assert!(all.contains(&ledger_key("wide", b"theme picture")));
        assert!(!all.contains(&ledger_key("wide", b"a picture the player chose")));
        assert!(!all.contains(&ledger_key("hero", b"theme picture")), "a hash counts for its own kind only");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn adding_a_folder_or_a_zip_puts_the_pack_in_place_and_replaces_it() {
        let root = tmp("add");
        let themes = root.join("themes");
        fs::create_dir_all(&themes).unwrap();
        let src = root.join("src");
        pack(&src, GOOD, &good_files());
        let added = add_from(&src, &themes, &themes.join(".incoming-a")).unwrap();
        assert_eq!((added.name.as_str(), added.themes), ("Camp Pack", 1));
        assert!(themes.join("camp-pack").join(MANIFEST).is_file());
        // the same pack again as a .zip with one top folder: replaced, nothing left over
        let mut z = ZipOut::new();
        z.add("Camp/theme.json", GOOD.as_bytes()).unwrap();
        for (n, d) in good_files() {
            z.add(&format!("Camp/{n}"), &d).unwrap();
        }
        let zip = root.join("camp.zip");
        fs::write(&zip, z.finish()).unwrap();
        add_from(&zip, &themes, &themes.join(".incoming-b")).unwrap();
        let _ = fs::remove_dir_all(themes.join(".incoming-b"));
        let names: Vec<String> = fs::read_dir(&themes).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        assert_eq!(names.iter().filter(|n| !n.starts_with(".incoming")).collect::<Vec<_>>(), ["camp-pack"]);
        // a broken pack changes nothing
        let bad = root.join("bad");
        pack(&bad, r#"{"format":1,"name":"Camp Pack","themes":[]}"#, &[]);
        assert!(add_from(&bad, &themes, &themes.join(".incoming-c")).is_err());
        assert!(check_pack(&themes.join("camp-pack")).is_ok(), "the earlier pack is untouched");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn the_example_pack_in_the_repository_is_valid() {
        let example = Path::new(env!("CARGO_MANIFEST_DIR")).join("../examples/theme-example");
        let m = check_pack(&example).unwrap();
        assert_eq!(slug(&m.name), "example-colours");
        assert_eq!(m.themes.len(), 2);
    }

    #[test]
    fn a_saved_theme_is_empty_or_pack_slash_id() {
        for ok in ["", "koi-s-camp/koi"] {
            assert!(key_ok(ok), "{ok}");
        }
        for bad in ["koi", "a/b/c", "../x/y", "a/", "/b", "A/b", "a/b\\c"] {
            assert!(!key_ok(bad), "{bad}");
        }
    }

    #[test]
    fn folder_names_are_plain() {
        assert_eq!(slug("Koi's Camp!"), "koi-s-camp");
        assert_eq!(slug("***"), "pack");
        assert!(id_ok("camp-1") && !id_ok("Camp") && !id_ok("a/b") && !id_ok(".x") && !id_ok(""));
    }
}
