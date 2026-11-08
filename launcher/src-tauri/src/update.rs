// SPDX-License-Identifier: GPL-3.0-or-later
//! Releases and the in-launcher update.
//!
//! `load_releases`: one GET of the GitHub releases list per session (plus one per refresh), the raw answer kept in the app config
//! folder and served when the network fails. Every failure is silent (one line on stderr, at worst an empty list).
//!
//! `install`: download, verify, extract, swap, all through `<install>/.update/`:
//! 1. SHA256SUMS.txt first, so a release without a checksum for the package costs no 170 MB. The package is streamed to `.update/`
//!    and hashed as it arrives. Nothing outside `.update/` has changed until step 3, so cancel and every failure here only delete it.
//! 2. The system tar unpacks it to `.update/staged/`, and the tree is checked: launcher and tools/build in `PokopiaRecomp-<v>/`.
//! 3. `apply`: each file of the package moves its old self to `.update/backup/` and takes its place, the launcher last. Files the
//!    package does not hold are never looked at, and it can never replace `KEEP` even if a release shipped one. Every step records
//!    its inverse; a failure runs them backwards. The `applying` marker covers a power cut: the next start restores the backup
//!    instead of deleting it.
//! 4. `relaunch`: the new launcher starts with `--updated` and this one exits. The running exe cannot be deleted on Windows but can
//!    be renamed, so it becomes `<name>.old` and the next start deletes it (`clean_start`).
//!
//! Renames only, inside one folder tree, so a swap cannot run out of disk space halfway. A second launcher window leaves a running
//! update alone: `.update/lock` is held for the whole update.
//! The pure parts take plain paths and no AppHandle: the tests below build a fake install and run them.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::cmp::Ordering;
use std::fs::{self, File, OpenOptions, TryLockError};
use std::io::{self, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering::SeqCst};
use std::sync::Mutex;
use std::time::Duration;

const REPO: &str = "featureJosh/pokopia-recomp";
/// The releases list: small, so a whole-call limit, generous for a slow link.
const API_TIMEOUT: Duration = Duration::from_secs(15);
/// The package is ~170 MB: connect and silence limits, never a total one.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
const READ_TIMEOUT: Duration = Duration::from_secs(30);
/// Builds from a checkout carry these (0.0.0 = untagged build, 0.1.1 = the Cargo.toml default); a tagged release stamps its own.
const DEV_VERSIONS: [&str; 2] = ["0.0.0", "0.1.1"];
/// Twenty releases with long notes and their asset lists; a cut JSON fails to parse and the cache is served.
const LIST_LIMIT: u64 = 8 << 20;

const UPDATE_DIR: &str = ".update";
const SUMS_ASSET: &str = "SHA256SUMS.txt";
const MARKER: &str = "applying";
const LOCK: &str = "lock";
const RUNTIME_DLL: &str = "pkp_rt.dll";
/// install_update's Err when the player cancelled, for the page to tell it from a failure.
pub const CANCELLED: &str = "Update cancelled.";

#[cfg(windows)]
const ASSET_SUFFIX: &str = "-win64.zip";
#[cfg(not(windows))]
const ASSET_SUFFIX: &str = "-linux-x64.tar.gz";
#[cfg(windows)]
const PACKAGE_LAUNCHER: &str = "PokopiaLauncher.exe";
#[cfg(not(windows))]
const PACKAGE_LAUNCHER: &str = "PokopiaLauncher.AppImage";

/// Top-level names that belong to the player. make-release.ps1 refuses to ship most of them; `apply` skips them all the same.
const KEEP: [&str; 8] = ["save", "pkp.ini", "profiles.json", "controls.ini", "shader_cache", "build", "nup_dump", UPDATE_DIR];

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Release {
    pub tag: String,
    pub name: String,
    /// when it was published, ISO 8601 as GitHub gives it
    pub date: String,
    /// the release notes, markdown
    pub body: String,
    pub url: String,
    /// a Testing build (a GitHub prerelease): shown only on the Testing channel
    pub prerelease: bool,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct UpdateInfo {
    /// the tag as published, e.g. "v0.3.0"
    pub tag: String,
    pub name: String,
    /// the release page
    pub url: String,
    /// bytes of the package for this platform
    pub size: u64,
}

#[derive(Serialize, Clone, Debug, PartialEq, Default)]
pub struct Releases {
    pub releases: Vec<Release>,
    pub update: Option<UpdateInfo>,
}

#[derive(Deserialize, Clone, Debug)]
pub struct Asset {
    name: String,
    #[serde(default)]
    size: u64,
    #[serde(default)]
    browser_download_url: String,
}

/// One entry of the API's answer.
#[derive(Deserialize, Clone, Debug)]
pub struct GhRelease {
    tag_name: String,
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    html_url: String,
    #[serde(default)]
    published_at: Option<String>,
    #[serde(default)]
    body: Option<String>,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    prerelease: bool,
    #[serde(default)]
    assets: Vec<Asset>,
}

impl GhRelease {
    fn title(&self) -> String {
        self.name.clone().filter(|n| !n.trim().is_empty()).unwrap_or_else(|| self.tag_name.clone())
    }
}

/// A parsed `major.minor.patch[-pre]` version (a leading `v` and `+build` metadata are ignored).
#[derive(Debug, PartialEq, Eq)]
pub struct Version {
    core: [u64; 3],
    pre: Vec<String>,
}

impl Version {
    pub fn parse(s: &str) -> Option<Version> {
        let s = s.trim();
        let s = s.strip_prefix('v').or_else(|| s.strip_prefix('V')).unwrap_or(s);
        let s = s.split('+').next()?;
        let (core, pre) = match s.split_once('-') {
            Some((c, p)) => (c, Some(p)),
            None => (s, None),
        };
        let mut nums = core.split('.');
        let mut core3 = [0u64; 3];
        for slot in core3.iter_mut() {
            let part = nums.next()?;
            if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
                return None;
            }
            *slot = part.parse().ok()?;
        }
        if nums.next().is_some() {
            return None;
        }
        let pre = match pre {
            None => Vec::new(),
            Some(p) => {
                let ids: Vec<String> = p.split('.').map(str::to_string).collect();
                if ids.iter().any(|i| i.is_empty() || !i.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')) {
                    return None;
                }
                ids
            }
        };
        Some(Version { core: core3, pre })
    }
}

impl Ord for Version {
    fn cmp(&self, other: &Self) -> Ordering {
        self.core.cmp(&other.core).then_with(|| match (self.pre.is_empty(), other.pre.is_empty()) {
            (true, true) => Ordering::Equal,
            (true, false) => Ordering::Greater, // a release is newer than its own prerelease
            (false, true) => Ordering::Less,
            (false, false) => {
                for (a, b) in self.pre.iter().zip(&other.pre) {
                    let o = match (a.parse::<u64>(), b.parse::<u64>()) {
                        (Ok(x), Ok(y)) => x.cmp(&y),
                        (Ok(_), Err(_)) => Ordering::Less, // numeric identifiers rank below alphanumeric ones
                        (Err(_), Ok(_)) => Ordering::Greater,
                        _ => a.cmp(b),
                    };
                    if o != Ordering::Equal {
                        return o;
                    }
                }
                self.pre.len().cmp(&other.pre.len())
            }
        })
    }
}
impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// True when `tag` is a valid version newer than `current`. Anything unparsable is "not newer".
pub fn is_newer(tag: &str, current: &str) -> bool {
    match (Version::parse(tag), Version::parse(current)) {
        (Some(t), Some(c)) => t > c,
        _ => false,
    }
}

pub fn is_dev_version(v: &str) -> bool {
    DEV_VERSIONS.contains(&v.trim())
}



fn github(url: &str) -> bool {
    url.starts_with("https://github.com/") && !url.chars().any(char::is_whitespace)
}

/// Which releases the player follows. Stable (the default) is what the launcher has always offered: published releases, no prereleases.
/// Testing also offers prereleases (`v0.8.1-beta.1`, built from the `testing` branch), newest first like any other release.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Channel {
    Stable,
    Testing,
}

impl Channel {
    /// launcher.json's `channel`; anything but "testing" (empty, a typo, a value from a newer launcher) is Stable.
    pub fn parse(s: &str) -> Channel {
        if s.trim().eq_ignore_ascii_case("testing") { Channel::Testing } else { Channel::Stable }
    }
    /// How many releases to ask GitHub for: Testing builds share the list, so it needs more room to keep the stable ones in view.
    pub fn list_size(self) -> u32 {
        match self {
            Channel::Stable => 20,
            Channel::Testing => 50,
        }
    }
}

/// The API's answer without drafts and anything that does not link to github.com. Prereleases are kept here: `for_channel` decides.
pub fn parse_releases(body: &[u8]) -> Result<Vec<GhRelease>, String> {
    let all: Vec<GhRelease> = serde_json::from_slice(body).map_err(|e| e.to_string())?;
    Ok(all.into_iter().filter(|r| !r.draft && !r.tag_name.is_empty() && github(&r.html_url)).collect())
}

/// The releases a channel sees. Stable drops prereleases, so for a Stable player nothing differs from before channels existed.
pub fn for_channel(list: Vec<GhRelease>, channel: Channel) -> Vec<GhRelease> {
    match channel {
        Channel::Stable => list.into_iter().filter(|r| !r.prerelease).collect(),
        Channel::Testing => list,
    }
}

/// This platform's package of a release.
fn asset_for<'a>(r: &'a GhRelease, suffix: &str) -> Option<&'a Asset> {
    r.assets.iter().find(|a| a.name.ends_with(suffix) && !a.name.contains(['/', '\\']) && github(&a.browser_download_url))
}

fn sums_of(r: &GhRelease) -> Option<&Asset> {
    r.assets.iter().find(|a| a.name == SUMS_ASSET && github(&a.browser_download_url))
}

/// The newest release newer than `current` that can be installed here. One without this platform's package or a checksum file is
/// passed over: the Linux package lands an hour after the Windows one, and an Update button that can only fail is worse than none
/// for that hour (What's New lists the release anyway).
fn pick_update<'a>(list: &'a [GhRelease], current: &str, suffix: &str) -> Option<(&'a GhRelease, &'a Asset, &'a Asset)> {
    list.iter()
        .filter(|r| is_newer(&r.tag_name, current))
        .filter_map(|r| Some((r, asset_for(r, suffix)?, sums_of(r)?)))
        .max_by_key(|(r, _, _)| Version::parse(&r.tag_name))
}

/// What get_releases returns. `current`: None for a build that takes no updates.
pub fn view(list: &[GhRelease], current: Option<&str>, suffix: &str) -> Releases {
    let releases = list
        .iter()
        .map(|r| Release {
            tag: r.tag_name.clone(),
            name: r.title(),
            date: r.published_at.clone().unwrap_or_default(),
            body: r.body.clone().unwrap_or_default(),
            url: r.html_url.clone(),
            prerelease: r.prerelease,
        })
        .collect();
    let update = current.and_then(|c| pick_update(list, c, suffix)).map(|(r, a, _)| UpdateInfo { tag: r.tag_name.clone(), name: r.title(), url: r.html_url.clone(), size: a.size });
    Releases { releases, update }
}

pub fn view_here(list: &[GhRelease], current: Option<&str>) -> Releases {
    view(list, current, ASSET_SUFFIX)
}

fn get(agent: &ureq::Agent, url: &str, limit: u64) -> Result<Vec<u8>, String> {
    let resp = agent.get(url).set("Accept", "application/vnd.github+json").call().map_err(|e| e.to_string())?;
    let mut body = Vec::new();
    resp.into_reader().take(limit).read_to_end(&mut body).map_err(|e| e.to_string())?;
    Ok(body)
}

fn user_agent(version: &str) -> String {
    format!("PokopiaRecomp-Launcher/{}", version)
}

/// The parsed list of this session.
#[derive(Default)]
pub struct ReleaseStore(Mutex<Option<Vec<GhRelease>>>);

/// The releases: this session's copy unless `refresh`, else one GET, then the file `cache` on failure, else nothing. The lock is
/// held over the GET, so the pages' calls at start make one request between them.
pub fn load_releases(store: &ReleaseStore, cache: &Path, version: &str, refresh: bool, channel: Channel) -> Vec<GhRelease> {
    let mut mem = store.0.lock().unwrap();
    if let (Some(list), false) = (mem.as_ref(), refresh) {
        return for_channel(list.clone(), channel);
    }
    let agent = ureq::AgentBuilder::new().timeout(API_TIMEOUT).user_agent(&user_agent(version)).build();
    let url = format!("https://api.github.com/repos/{}/releases?per_page={}", REPO, channel.list_size());
    let fetched = get(&agent, &url, LIST_LIMIT).and_then(|body| parse_releases(&body).map(|list| (body, list)));
    let list = match fetched {
        Ok((body, list)) => {
            if let Err(e) = crate::settings::write_atomic(cache, &String::from_utf8_lossy(&body)) {
                eprintln!("release list not cached: {}", e);
            }
            list
        }
        Err(e) => {
            eprintln!("release check skipped: {}", e);
            fs::read(cache).ok().and_then(|b| parse_releases(&b).ok()).or_else(|| mem.take()).unwrap_or_default()
        }
    };
    *mem = Some(list.clone());
    for_channel(list, channel)
}

/// What `install` fetches.
#[derive(Clone, Debug, PartialEq)]
pub struct Target {
    pub tag: String,
    pub asset: String,
    pub asset_url: String,
    pub size: u64,
    pub sums_url: String,
}

pub fn target_of(list: &[GhRelease], current: &str, suffix: &str) -> Option<Target> {
    pick_update(list, current, suffix).map(|(r, a, s)| Target {
        tag: r.tag_name.clone(),
        asset: a.name.clone(),
        asset_url: a.browser_download_url.clone(),
        size: a.size,
        sums_url: s.browser_download_url.clone(),
    })
}

pub fn target_here(list: &[GhRelease], current: &str) -> Option<Target> {
    target_of(list, current, ASSET_SUFFIX)
}

/// The SHA-256 SHA256SUMS.txt lists for `name` (`<hex> *<name>` from the release job, `<hex>  <name>` from sha256sum), lower case.
pub fn sum_for(sums: &str, name: &str) -> Option<String> {
    sums.trim_start_matches('\u{feff}').lines().find_map(|l| {
        let (hex, rest) = l.trim().split_once(char::is_whitespace)?;
        let file = rest.trim_start().trim_start_matches('*');
        (file == name && hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_hexdigit())).then(|| hex.to_ascii_lowercase())
    })
}

/// Room for the download and its unpacked copy. Unknown space passes: extraction would report a full drive anyway.
pub fn enough_space(free: Option<u64>, asset_size: u64) -> bool {
    free.is_none_or(|f| f >= asset_size.saturating_mul(2))
}

/// `update-progress` events for one phase: the first, the last, and one per percent in between.
#[derive(Default)]
struct Throttle {
    phase: String,
    last: u64,
}

impl Throttle {
    fn pass(&mut self, phase: &str, done: u64, total: u64) -> bool {
        if phase != self.phase || done == total || done.saturating_sub(self.last) >= (total / 100).max(1) {
            self.phase = phase.to_string();
            self.last = done;
            return true;
        }
        false
    }
}

/// Copies at most `size` + 1 bytes (one more shows an oversized file) to `to`, hashing them: (bytes, lower-case SHA-256).
fn hash_copy(from: &mut dyn Read, to: &mut dyn Write, size: u64, cancel: &AtomicBool, progress: &mut dyn FnMut(u64)) -> Result<(u64, String), String> {
    let (mut hash, mut n, mut buf) = (Sha256::new(), 0u64, vec![0u8; 1 << 16]);
    let mut from = from.take(size.saturating_add(1));
    loop {
        if cancel.load(SeqCst) {
            return Err(CANCELLED.into());
        }
        let k = from.read(&mut buf).map_err(|e| format!("The download failed: {}. Nothing was changed. Try again.", e))?;
        if k == 0 {
            break;
        }
        hash.update(&buf[..k]);
        to.write_all(&buf[..k]).map_err(|e| format!("Could not save the update: {}. Nothing was changed.", e))?;
        n += k as u64;
        progress(n.min(size));
    }
    Ok((n, format!("{:x}", hash.finalize())))
}

/// The system's tar: bsdtar in System32 on Windows, which reads the zip too (never one found on PATH).
fn tar() -> Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        let root = std::env::var_os("SystemRoot").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"C:\Windows"));
        let mut cmd = Command::new(root.join("System32").join("tar.exe"));
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        cmd
    }
    #[cfg(not(windows))]
    {
        crate::host::command("tar")
    }
}

/// Unpacks `dir/archive` into a new `dir/out`. Names relative to `dir`: Windows' tar cannot open a path with characters outside the
/// system code page, and an install in such a folder must update too. Its error output goes to a file so a full pipe can never stall it.
pub fn extract(dir: &Path, archive: &str, out: &str, cancel: &AtomicBool) -> Result<(), String> {
    fs::create_dir_all(dir.join(out)).map_err(|e| format!("Could not unpack the update: {}. Nothing was changed.", e))?;
    let log = dir.join("tar.log");
    let err = File::create(&log).map_err(|e| format!("Could not unpack the update: {}. Nothing was changed.", e))?;
    let mut child = tar()
        .current_dir(dir)
        .arg("-xf")
        .arg(archive)
        .arg("-C")
        .arg(out)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(err)
        .spawn()
        .map_err(|e| format!("Could not start the system's tar to unpack the update: {}. Nothing was changed.", e))?;
    let status = loop {
        if cancel.load(SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(CANCELLED.into());
        }
        match child.try_wait().map_err(|e| e.to_string())? {
            Some(s) => break s,
            None => std::thread::sleep(Duration::from_millis(100)),
        }
    };
    if status.success() {
        return Ok(());
    }
    let text = fs::read_to_string(&log).unwrap_or_default();
    let last = text.lines().last().unwrap_or("").trim().to_string();
    Err(format!("Could not unpack the update ({}). Check that the drive has free space, then try again. Nothing was changed.", if last.is_empty() { status.to_string() } else { last }))
}

/// `staged/PokopiaRecomp-<version>`, once it has the launcher and tools/build. Asked before the install is touched.
pub fn find_package(staged: &Path, version: &str, launcher: &str) -> Result<PathBuf, String> {
    let root = staged.join(format!("PokopiaRecomp-{}", version));
    if root.join(launcher).is_file() && root.join("tools").join("build").is_dir() {
        return Ok(root);
    }
    Err("The downloaded package is not laid out as expected. Nothing was changed.".into())
}

/// Windows: a virus scanner holds a file it just saw (the extracted launcher above all) for a moment, so a rename waits a little.
fn retryable(e: &io::Error) -> bool {
    cfg!(windows) && matches!(e.raw_os_error(), Some(5 | 32 | 33)) // access denied, sharing and lock violation
}

fn mv(from: &Path, to: &Path) -> io::Result<()> {
    let mut tries = 0;
    loop {
        match fs::rename(from, to) {
            Err(e) if tries < 20 && retryable(&e) => {
                tries += 1;
                std::thread::sleep(Duration::from_millis(150));
            }
            r => return r,
        }
    }
}

fn exists(p: &Path) -> bool {
    fs::symlink_metadata(p).is_ok()
}

fn with_old(p: &Path) -> PathBuf {
    let mut s = p.as_os_str().to_os_string();
    s.push(".old");
    PathBuf::from(s)
}

/// Every file (and symlink) under `dir`, relative to it, sorted.
fn walk(dir: &Path) -> io::Result<Vec<PathBuf>> {
    let (mut out, mut stack) = (Vec::new(), vec![PathBuf::new()]);
    while let Some(rel) = stack.pop() {
        for e in fs::read_dir(dir.join(&rel))? {
            let e = e?;
            let r = rel.join(e.file_name());
            if e.file_type()?.is_dir() {
                stack.push(r);
            } else {
                out.push(r);
            }
        }
    }
    out.sort();
    Ok(out)
}

fn kept(rel: &Path) -> bool {
    match rel.components().next() {
        Some(Component::Normal(top)) => KEEP.iter().any(|k| top.to_string_lossy().eq_ignore_ascii_case(k)),
        _ => true,
    }
}

/// One undo step: the inverse of something `apply` did.
#[derive(Debug)]
enum Undo {
    Move { from: PathBuf, to: PathBuf },
    Remove(PathBuf),
    RemoveDir(PathBuf),
}

/// Runs the steps backwards. Returns what could not be put back.
fn undo_all(steps: Vec<Undo>) -> Vec<String> {
    let mut failed = Vec::new();
    for s in steps.into_iter().rev() {
        match s {
            Undo::Move { from, to } => {
                if let Err(e) = mv(&from, &to) {
                    failed.push(format!("{}: {}", to.display(), e));
                }
            }
            Undo::Remove(p) => {
                if let Err(e) = fs::remove_file(&p) {
                    if e.kind() != io::ErrorKind::NotFound {
                        failed.push(format!("{}: {}", p.display(), e));
                    }
                }
            }
            Undo::RemoveDir(p) => drop(fs::remove_dir(&p)), // only an empty one goes; an extra empty folder would harm nothing
        }
    }
    failed
}

fn rollback_message(failed: &[String]) -> String {
    format!("{} files could not be put back (first: {}). Close the launcher and start it again; if this stays, extract the release again.", failed.len(), failed[0])
}

/// What `apply` works on. Plain paths, so the tests run it on a fake install.
pub struct Job<'a> {
    /// the install: the launcher's folder
    pub root: &'a Path,
    /// the unpacked `PokopiaRecomp-<v>/`
    pub package: &'a Path,
    /// `.update/`, on the same drive as `root`
    pub work: &'a Path,
    /// the launcher's name in the package
    pub launcher_name: &'a str,
    /// the launcher's path in the install, which the player may have renamed
    pub launcher: &'a Path,
    /// the old pkp_rt.dll of a custom game folder: pkp.exe there loads this copy
    pub game_dll: Option<PathBuf>,
}

/// A finished swap, which can still be undone.
#[derive(Debug)]
pub struct Applied(Vec<Undo>);

fn make_parents(dir: &Path, undo: &mut Vec<Undo>) -> io::Result<()> {
    let missing: Vec<&Path> = dir.ancestors().take_while(|a| !exists(a)).collect();
    for d in missing.into_iter().rev() {
        fs::create_dir(d)?;
        undo.push(Undo::RemoveDir(d.to_path_buf()));
    }
    Ok(())
}

fn parent_of(p: &Path) -> &Path {
    p.parent().unwrap_or(Path::new(""))
}

type Mover<'m> = &'m mut dyn FnMut(&Path, &Path) -> io::Result<()>;

fn step(from: &Path, to: &Path, undo: &mut Vec<Undo>, mover: &mut Mover) -> io::Result<()> {
    mover(from, to)?;
    undo.push(Undo::Move { from: to.to_path_buf(), to: from.to_path_buf() });
    Ok(())
}

fn swap(job: &Job, undo: &mut Vec<Undo>, files: &[PathBuf], report: &mut dyn FnMut(&str, u64, u64), mover: &mut Mover) -> io::Result<()> {
    let backup = job.work.join("backup");
    let total = files.len() as u64 + 1;
    for (i, rel) in files.iter().enumerate() {
        report("apply", i as u64, total);
        let dst = job.root.join(rel);
        make_parents(parent_of(&dst), undo)?;
        if exists(&dst) {
            let bak = backup.join(rel);
            fs::create_dir_all(parent_of(&bak))?; // inside .update/, removed with it
            step(&dst, &bak, undo, mover)?;
        }
        step(&job.package.join(rel), &dst, undo, mover)?;
    }
    if let Some(dst) = &job.game_dll {
        let src = job.root.join(RUNTIME_DLL);
        if src.is_file() {
            // possibly another drive, and the install keeps its own: a copy, with the old file set aside beside it
            let old = with_old(dst);
            let _ = fs::remove_file(&old);
            step(dst, &old, undo, mover)?;
            undo.push(Undo::Remove(dst.clone()));
            fs::copy(&src, dst)?;
        }
    }
    // last, so an install cut short by a power failure still starts the launcher that can restore it
    let (src, dst) = (job.package.join(job.launcher_name), job.launcher);
    let old = with_old(dst);
    let _ = fs::remove_file(&old);
    if exists(dst) {
        step(dst, &old, undo, mover)?;
    }
    step(&src, dst, undo, mover)?;
    report("apply", total, total);
    Ok(())
}

pub fn apply(job: &Job, report: &mut dyn FnMut(&str, u64, u64)) -> Result<Applied, String> {
    apply_with(job, report, &mut |a, b| mv(a, b))
}

/// `apply` with the move of the forward steps passed in, so a test can fail any one of them.
fn apply_with(job: &Job, report: &mut dyn FnMut(&str, u64, u64), mut mover: Mover) -> Result<Applied, String> {
    let unreadable = |e: io::Error| format!("Could not read the unpacked update: {}. Nothing was changed.", e);
    let files: Vec<PathBuf> = walk(job.package).map_err(unreadable)?.into_iter().filter(|r| !kept(r) && r != Path::new(job.launcher_name)).collect();
    // a folder where the package has a file: the one conflict moving aside must not resolve, it would take the folder's content along
    if let Some(r) = files.iter().find(|r| job.root.join(r).is_dir()) {
        return Err(format!("{} is a folder in your game folder, but the update has a file of that name. Nothing was changed.", r.display()));
    }
    let marker = job.work.join(MARKER);
    fs::write(&marker, "").map_err(|e| format!("Could not write in the update folder: {}. Nothing was changed.", e))?;
    let mut undo = Vec::new();
    match swap(job, &mut undo, &files, report, &mut mover) {
        Ok(()) => {
            let _ = fs::remove_file(&marker);
            Ok(Applied(undo))
        }
        Err(e) => {
            let failed = undo_all(undo);
            if failed.is_empty() {
                let _ = fs::remove_file(&marker);
                Err(format!("The update could not be put in place: {}. Everything was restored.", e))
            } else {
                Err(format!("The update could not be put in place: {}. {}", e, rollback_message(&failed))) // the marker stays: the next start tries again
            }
        }
    }
}

/// A start found the `applying` marker: a swap was cut off. Puts the backup back over the install. Returns how many files stayed behind.
/// ponytail: only the backup. A launcher cut off in its own last two moves, or a custom folder's dll, stays new: both still work, and
/// the old pkp.exe with a new runtime is the state every update leaves until the rebuild anyway.
fn recover(root: &Path, work: &Path) -> usize {
    let backup = work.join("backup");
    let files = walk(&backup).unwrap_or_default();
    files.iter().filter(|rel| mv(&backup.join(rel), &root.join(rel)).is_err()).count()
}

/// Another launcher is in the middle of an update here.
fn locked(work: &Path) -> bool {
    OpenOptions::new().write(true).open(work.join(LOCK)).is_ok_and(|f| matches!(f.try_lock(), Err(TryLockError::WouldBlock)))
}

/// Start of a GUI run: finishes off the last update. Waits for the launcher that restarted us, which holds `.update/lock` and its own
/// exe until it has exited. Runs on a thread of its own: deleting the old tools/ takes seconds, and `install` waits for it.
/// `old` is the exact list of `.old` files an update makes: a plain `*.old` here would also delete a player's `pkp.ini.old`.
pub fn clean_start(root: &Path, old: &[PathBuf]) {
    let work = root.join(UPDATE_DIR);
    let mut tries = 0;
    while locked(&work) {
        tries += 1;
        if tries > 40 {
            return; // a second window, with an update of its own running
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    let stuck = work.join(MARKER).is_file() && recover(root, &work) > 0;
    if work.is_dir() && !stuck {
        let _ = fs::remove_dir_all(&work);
    }
    for _ in 0..40 {
        let left: Vec<&PathBuf> = old.iter().filter(|p| exists(p)).collect();
        if left.is_empty() {
            break;
        }
        for p in left {
            let _ = fs::remove_file(p);
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

/// The `.old` files of `launcher` and of a custom game folder.
pub fn old_files(launcher: &Path, game_dir: &Path) -> Vec<PathBuf> {
    vec![with_old(launcher), with_old(&game_dir.join(RUNTIME_DLL))]
}

/// The update's flags. `busy` also keeps Play and Set up from starting under it.
#[derive(Default)]
pub struct UpdateState {
    busy: AtomicBool,
    cancel: AtomicBool,
    /// files are moving: the window must not close
    applying: AtomicBool,
    /// `clean_start` runs
    cleaning: AtomicBool,
}

impl UpdateState {
    pub fn busy(&self) -> bool {
        self.busy.load(SeqCst)
    }
    pub fn applying(&self) -> bool {
        self.applying.load(SeqCst)
    }
    pub fn cancel(&self) {
        self.cancel.store(true, SeqCst);
    }
    pub fn set_cleaning(&self, on: bool) {
        self.cleaning.store(on, SeqCst);
    }
    /// None: an update already runs.
    pub fn claim(&self) -> Option<Claim<'_>> {
        if self.busy.swap(true, SeqCst) {
            return None;
        }
        self.cancel.store(false, SeqCst);
        Some(Claim(self))
    }
}

/// Clears `busy` and `applying` when the update ends, however it ends.
pub struct Claim<'a>(&'a UpdateState);

impl Drop for Claim<'_> {
    fn drop(&mut self) {
        self.0.applying.store(false, SeqCst);
        self.0.busy.store(false, SeqCst);
    }
}

pub struct Paths {
    pub install: PathBuf,
    /// the running launcher
    pub launcher: PathBuf,
    pub game_dir: PathBuf,
}

/// The game's pkp_rt.dll when the game folder is not the install.
fn game_dll(p: &Paths) -> Option<PathBuf> {
    let canon = |d: &Path| fs::canonicalize(d).unwrap_or_else(|_| d.to_path_buf());
    let dll = p.game_dir.join(RUNTIME_DLL);
    (canon(&p.game_dir) != canon(&p.install) && dll.is_file()).then_some(dll)
}

/// A swap that is done, with the lock still held.
pub struct Session {
    applied: Applied,
    work: PathBuf,
    lock: Option<File>,
}

impl Session {
    /// The new launcher runs: the backup can go.
    pub fn finish(mut self) {
        drop(self.lock.take());
        let _ = fs::remove_dir_all(&self.work);
    }

    /// The new launcher did not start: back to the old install. Err: what could not be put back.
    pub fn rollback(mut self) -> Result<(), String> {
        let failed = undo_all(std::mem::take(&mut self.applied.0));
        drop(self.lock.take());
        if failed.is_empty() {
            let _ = fs::remove_dir_all(&self.work);
            return Ok(());
        }
        let _ = fs::write(self.work.join(MARKER), "");
        Err(rollback_message(&failed))
    }
}

fn reset_work(root: &Path, work: &Path) -> Result<(), String> {
    if locked(work) {
        return Err("Another launcher window is updating. Close it first.".into());
    }
    if work.join(MARKER).is_file() && recover(root, work) > 0 {
        return Err("An earlier update did not finish and could not be undone. Extract the release again.".into());
    }
    if exists(work) {
        fs::remove_dir_all(work).map_err(|e| format!("Could not clear {}: {}.", work.display(), e))?;
    }
    fs::create_dir_all(work).map_err(|e| format!("The launcher cannot write in {}: {}. Nothing was changed.", root.display(), e))
}

/// Steps 1 to 3 of the header. `ready` says whether the install may still change (the game or setup may have started meanwhile), right
/// before the swap. Err: nothing changed, unless it says otherwise. `report` gets (phase, done, total), about 100 per phase.
pub fn install(t: &Target, p: &Paths, st: &UpdateState, report: &mut dyn FnMut(&str, u64, u64), ready: &dyn Fn() -> Result<(), String>) -> Result<Session, String> {
    while st.cleaning.load(SeqCst) {
        if st.cancel.load(SeqCst) {
            return Err(CANCELLED.into());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let work = p.install.join(UPDATE_DIR);
    reset_work(&p.install, &work)?;
    let lock = File::create(work.join(LOCK)).ok().filter(|f| f.try_lock().is_ok()).ok_or("Another launcher window is updating. Close it first.")?;
    match stages(t, p, st, &work, report, ready) {
        Ok(applied) => Ok(Session { applied, work, lock: Some(lock) }),
        Err(e) => {
            drop(lock);
            // a failed rollback keeps the marker and the backup for the next start
            if !work.join(MARKER).is_file() {
                let _ = fs::remove_dir_all(&work);
            }
            Err(e)
        }
    }
}

fn stages(t: &Target, p: &Paths, st: &UpdateState, work: &Path, report: &mut dyn FnMut(&str, u64, u64), ready: &dyn Fn() -> Result<(), String>) -> Result<Applied, String> {
    let free = crate::preflight::free_bytes(&p.install);
    if !enough_space(free, t.size) {
        let gb = |b: u64| format!("{:.1} GB", b as f64 / crate::preflight::GB as f64);
        return Err(format!("Not enough free disk space on {}: the update needs about {} and {} is free.", crate::preflight::drive_of(&p.install), gb(t.size.saturating_mul(2)), gb(free.unwrap_or(0))));
    }
    let mut gate = Throttle::default();
    let mut send = |phase: &str, done: u64, total: u64| {
        if gate.pass(phase, done, total) {
            report(phase, done, total);
        }
    };
    send("download", 0, t.size);
    let agent = ureq::AgentBuilder::new().timeout_connect(CONNECT_TIMEOUT).timeout_read(READ_TIMEOUT).user_agent(&user_agent(env!("CARGO_PKG_VERSION"))).build();
    let failed = |e: String| format!("The download failed: {}. Nothing was changed. Try again.", e);
    let sums = String::from_utf8_lossy(&get(&agent, &t.sums_url, 1 << 16).map_err(failed)?).into_owned();
    let want = sum_for(&sums, &t.asset).ok_or("This release has no checksum for its download yet. Try again in a while. Nothing was changed.")?;

    let archive = work.join(&t.asset);
    let resp = agent.get(&t.asset_url).call().map_err(|e| failed(e.to_string()))?;
    let save_failed = |e: io::Error| format!("Could not save the update: {}. Nothing was changed.", e);
    let mut out = File::create(&archive).map_err(save_failed)?;
    let (n, got) = hash_copy(&mut resp.into_reader(), &mut out, t.size, &st.cancel, &mut |n| send("download", n, t.size))?;
    out.sync_all().map_err(save_failed)?;
    drop(out);
    send("verify", 0, 1);
    if n != t.size || got != want {
        return Err("The download arrived damaged. Nothing was changed. Try again.".into());
    }
    send("verify", 1, 1);

    send("extract", 0, 1);
    extract(work, &t.asset, "staged", &st.cancel)?;
    send("extract", 1, 1);
    let version = t.tag.trim_start_matches(['v', 'V']);
    let package = find_package(&work.join("staged"), version, PACKAGE_LAUNCHER)?;

    // the last moment to turn back: the download took minutes and Steam can start the game without the launcher
    if st.cancel.load(SeqCst) {
        return Err(CANCELLED.into());
    }
    ready()?;
    st.applying.store(true, SeqCst);
    let job = Job { root: &p.install, package: &package, work, launcher_name: PACKAGE_LAUNCHER, launcher: &p.launcher, game_dll: game_dll(p) };
    apply(&job, &mut send)
}

/// Starts the launcher now at `path` with `--updated`. host::command keeps a new AppImage from inheriting the old one's mount. A launcher
/// run with --appimage-extract-and-run (no FUSE) has no mount either, and the new one needs the same way in: the variable says it.
pub fn relaunch(path: &Path, dir: &Path) -> io::Result<()> {
    let mut cmd = crate::host::command(path);
    #[cfg(not(windows))]
    if std::env::var("APPDIR").is_ok_and(|d| !d.is_empty() && !d.contains(".mount_")) {
        cmd.env("APPIMAGE_EXTRACT_AND_RUN", "1");
    }
    cmd.arg("--updated").current_dir(dir).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    cmd.spawn().map(drop)
}

#[cfg(test)]
mod channel_tests {
    use super::*;

    fn rel(tag: &str, prerelease: bool) -> String {
        let asset = format!("PokopiaRecomp-{}-linux-x64.tar.gz", tag.trim_start_matches('v'));
        format!(
            r#"{{"tag_name":"{tag}","name":"{tag}","html_url":"https://github.com/featureJosh/Recompia/releases/tag/{tag}","draft":false,"prerelease":{prerelease},"assets":[{{"name":"{asset}","size":1,"browser_download_url":"https://github.com/featureJosh/Recompia/releases/download/{tag}/{asset}"}},{{"name":"SHA256SUMS.txt","size":1,"browser_download_url":"https://github.com/featureJosh/Recompia/releases/download/{tag}/SHA256SUMS.txt"}}]}}"#
        )
    }

    fn list_of(items: &[(&str, bool)]) -> Vec<GhRelease> {
        let body = format!("[{}]", items.iter().map(|(t, p)| rel(t, *p)).collect::<Vec<_>>().join(","));
        parse_releases(body.as_bytes()).unwrap()
    }

    #[test]
    fn channel_names() {
        assert_eq!(Channel::parse("testing"), Channel::Testing);
        assert_eq!(Channel::parse(" Testing "), Channel::Testing);
        for s in ["", "stable", "beta", "TESTINGS", "alpha"] {
            assert_eq!(Channel::parse(s), Channel::Stable, "{s:?}");
        }
        assert_eq!(Channel::Stable.list_size(), 20);
        assert!(Channel::Testing.list_size() > 20);
    }

    #[test]
    fn prereleases_rank_between_the_stable_releases() {
        assert!(is_newer("v0.8.1-beta.1", "0.8.0"));
        assert!(!is_newer("v0.8.1-beta.1", "0.8.1"), "a release is newer than its own prerelease");
        assert!(is_newer("v0.8.1", "0.8.1-beta.9"));
        assert!(is_newer("v0.8.1-beta.10", "0.8.1-beta.2"));
        assert!(!is_newer("v0.8.0", "0.8.1-beta.1"), "no downgrade from Testing back to an older stable");
    }

    #[test]
    fn stable_sees_exactly_what_it_saw_before_channels() {
        let all = list_of(&[("v0.8.1-beta.1", true), ("v0.8.0", false), ("v0.7.9", false), ("v0.7.9-rc.1", true)]);
        let tags: Vec<String> = for_channel(all, Channel::Stable).into_iter().map(|r| r.tag_name).collect();
        assert_eq!(tags, ["v0.8.0", "v0.7.9"]);
    }

    #[test]
    fn testing_sees_everything_that_is_not_a_draft() {
        let mut body = format!("[{}]", [rel("v0.8.1-beta.1", true), rel("v0.8.0", false)].join(","));
        body = body.replacen(r#""draft":false,"prerelease":true"#, r#""draft":true,"prerelease":true"#, 1);
        let all = parse_releases(body.as_bytes()).unwrap();
        assert_eq!(all.len(), 1, "a draft never shows");
        let all = list_of(&[("v0.8.1-beta.2", true), ("v0.8.1-beta.1", true), ("v0.8.0", false)]);
        assert_eq!(for_channel(all, Channel::Testing).len(), 3);
    }

    #[test]
    fn updates_follow_the_channel() {
        let all = list_of(&[("v0.8.1-beta.2", true), ("v0.8.1-beta.1", true), ("v0.8.0", false)]);
        let stable = for_channel(all.clone(), Channel::Stable);
        assert!(pick_update(&stable, "0.8.0", "-linux-x64.tar.gz").is_none(), "Stable never offers a prerelease");
        let testing = for_channel(all, Channel::Testing);
        let (r, _, _) = pick_update(&testing, "0.8.0", "-linux-x64.tar.gz").unwrap();
        assert_eq!(r.tag_name, "v0.8.1-beta.2");
        assert!(pick_update(&testing, "0.8.1-beta.2", "-linux-x64.tar.gz").is_none());
        let v = view(&testing, Some("0.8.0"), "-linux-x64.tar.gz");
        assert!(v.releases.iter().any(|r| r.prerelease) && v.releases.iter().any(|r| !r.prerelease));
    }

    #[test]
    fn a_testing_player_moves_on_to_the_final_release() {
        let all = list_of(&[("v0.8.1", false), ("v0.8.1-beta.2", true)]);
        let testing = for_channel(all, Channel::Testing);
        let (r, _, _) = pick_update(&testing, "0.8.1-beta.2", "-linux-x64.tar.gz").unwrap();
        assert_eq!(r.tag_name, "v0.8.1");
    }
}
