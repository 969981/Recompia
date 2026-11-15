// SPDX-License-Identifier: GPL-3.0-or-later
//! The in-launcher update of the release releases.rs picked.
//!
//! `install`: download, verify, extract, prepare, swap, all through `<install>/.update/`:
//! 1. SHA256SUMS.txt first, so a release without a checksum for the package costs no 170 MB. The package is streamed to `.update/`
//!    and hashed as it arrives. Nothing outside `.update/` has changed until step 4, so cancel and every failure before it only
//!    delete it.
//! 2. The system tar unpacks it to `.update/staged/`, and the tree is checked: launcher and tools/build in `PokopiaRecomp-<v>/`.
//! 3. `GameCode::prepare` (update_game.rs): the package's own build tools build the game code it needs into the game folder's
//!    `build/next`, beside the installed build, which stays playable; Play is not blocked until step 4. The part that takes long
//!    (minutes to an hour) is here, where it can be cancelled and continues from the object cache when Update is pressed again.
//!    Then the update waits until the game is closed (`wait_idle`).
//! 4. `apply` (update_apply.rs): each file of the package takes the place of its old self, which goes to `.update/backup/`; every
//!    step can be undone, and a start after a power cut restores the backup. Then `GameCode::adopt` moves the prepared build into
//!    place (renames, seconds); if it fails, the swap is undone.
//! 5. `relaunch`: the new launcher starts with `--updated` and this one exits. The running exe cannot be deleted on Windows but can
//!    be renamed, so it becomes `<name>.old` and the next start deletes it (`clean_start`). If the new launcher does not start,
//!    `Session::rollback` puts the old game build back and undoes the swap.
//!
//! Renames only, inside one folder tree, so a swap cannot run out of disk space halfway. A second launcher window leaves a running
//! update alone: `.update/lock` is held for the whole update.
//! The pure parts take plain paths and no AppHandle: the tests below build a fake install and run them.

use crate::download;
use crate::update_apply::{apply, exists, recover, rollback_message, with_old, Applied, Job, MARKER};
use crate::releases::{get, user_agent, Target};
use std::fs::{self, File, OpenOptions, TryLockError};
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use serde::Serialize;
use std::sync::atomic::{AtomicBool, Ordering::SeqCst};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

/// The package is ~170 MB: connect and silence limits, never a total one.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
const READ_TIMEOUT: Duration = Duration::from_secs(30);

pub const UPDATE_DIR: &str = ".update";
const LOCK: &str = "lock";
/// install_update's Err when the player cancelled, for the page to tell it from a failure.
pub const CANCELLED: &str = "Update cancelled.";
/// How often a prepared update looks whether the game has been closed.
const WAIT_POLL: Duration = Duration::from_millis(500);

#[cfg(windows)]
const PACKAGE_LAUNCHER: &str = "PokopiaLauncher.exe";
#[cfg(target_os = "macos")]
// The binary at the root of the release zip's PokopiaRecomp-<version>/ folder: that folder is the install root (exe_dir), so
// the package replaces the launcher and tools/ in place, as on Windows and Linux.
const PACKAGE_LAUNCHER: &str = "PokopiaLauncher";
#[cfg(not(any(windows, target_os = "macos")))]
const PACKAGE_LAUNCHER: &str = "PokopiaLauncher.AppImage";


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

/// Where a running update is, for the page (the `update-progress` event).
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Progress {
    /// "download", "verify", "extract", "prepare", "wait", "apply", "adopt" or "restart"
    pub phase: String,
    /// while preparing: the game build's own phase ("verify", "lift", "compile", "link", "finish"); empty otherwise
    pub step: String,
    pub done: u64,
    pub total: u64,
}

impl Progress {
    pub fn of(phase: &str, step: &str, done: u64, total: u64) -> Progress {
        Progress { phase: phase.into(), step: step.into(), done, total }
    }
}

/// `update-progress` events for one phase and step: the first, the last, and one per percent in between.
#[derive(Default)]
struct Throttle {
    key: (String, String),
    last: u64,
}

impl Throttle {
    fn pass(&mut self, p: &Progress) -> bool {
        let new = self.key.0 != p.phase || self.key.1 != p.step;
        if new || p.done == p.total || p.done.saturating_sub(self.last) >= (p.total / 100).max(1) {
            self.key = (p.phase.clone(), p.step.clone());
            self.last = p.done;
            return true;
        }
        false
    }
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
    // Every host's zip puts the same tree under PokopiaRecomp-<version>/: the launcher, tools/ and the runtime library. That
    // folder is the install root, and an update replaces what is in it in place.
    let root = staged.join(format!("PokopiaRecomp-{}", version));
    if root.join(launcher).is_file() && root.join("tools").join("build").is_dir() {
        return Ok(root);
    }
    Err("The downloaded package is not laid out as expected. Nothing was changed.".into())
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
    vec![with_old(launcher), with_old(&crate::host::runtime_lib(game_dir))]
}

/// The update's flags. `busy` keeps Set up and the other jobs that write what an update replaces from starting under it; Play only
/// waits for `applying`, so the game stays playable while the update downloads and prepares.
#[derive(Default)]
pub struct UpdateState {
    busy: AtomicBool,
    cancel: AtomicBool,
    /// the swap has begun (files are moving, then the launcher restarts): the window must not close and Play is refused
    applying: AtomicBool,
    /// `clean_start` runs
    cleaning: AtomicBool,
    /// Play holds it from its check to the game's start, the update while it decides that the swap begins (`wait_idle`): a game
    /// is either seen running by the update or refused by Play, never started between the two
    gate: Mutex<()>,
}

impl UpdateState {
    pub fn busy(&self) -> bool {
        self.busy.load(SeqCst)
    }
    pub fn applying(&self) -> bool {
        self.applying.load(SeqCst)
    }
    /// Play's hold on the swap (`gate`): while held, an update cannot begin to move files.
    pub fn hold_swap(&self) -> MutexGuard<'_, ()> {
        self.gate.lock().unwrap_or_else(|e| e.into_inner())
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
    let dll = crate::host::runtime_lib(&p.game_dir);
    (canon(&p.game_dir) != canon(&p.install) && dll.is_file()).then_some(dll)
}

/// The game code an update brings (release model b; update_game.rs builds it with the game build). Before the swap it is built with
/// the downloaded package's tools while the installed game stays playable; right after the swap it goes into place. The tests pass
/// their own.
pub trait GameCode {
    /// Builds the next game code with the build tools in `tools` (the unpacked package's), reporting (step, done, total). Ok(false):
    /// cancelled through `cancel`; what was built is kept, so the next update continues from it.
    fn prepare(&self, tools: &Path, cancel: &AtomicBool, report: &mut dyn FnMut(&str, u64, u64)) -> Result<bool, String>;
    /// Right after the swap, with the new build tools in `tools`: the prepared game code goes into place.
    fn adopt(&self, tools: &Path) -> Result<(), String>;
    /// The swap is being undone after `adopt`: the game code it replaced comes back.
    fn revert(&self, tools: &Path) -> Result<(), String>;
    /// The new launcher runs: what `revert` would have needed can go.
    fn finish(&self);
}

/// What the update asks of the launcher around the swap.
pub struct Steps<'a> {
    /// Some(why) while the install must not change under something running (the game, Set up): the swap waits for it
    pub busy: &'a dyn Fn() -> Option<String>,
    /// right before the swap, once nothing is busy (an older build's data copy, rollback.rs). Err stops the update.
    pub ready: &'a dyn Fn() -> Result<(), String>,
    pub code: &'a dyn GameCode,
}

/// A swap that is done, with the lock still held.
pub struct Session {
    applied: Applied,
    work: PathBuf,
    /// the install's tools folder, which holds the new build tools now
    tools: PathBuf,
    lock: Option<File>,
}

impl Session {
    /// The new launcher runs: the backup and the game code the update replaced can go.
    pub fn finish(mut self, code: &dyn GameCode) {
        code.finish();
        drop(self.lock.take());
        let _ = fs::remove_dir_all(&self.work);
    }

    /// The new launcher did not start: back to the old install, the game code first (with the new tools, before they go). Err: what
    /// could not be put back.
    pub fn rollback(mut self, code: &dyn GameCode) -> Result<(), String> {
        let reverted = code.revert(&self.tools);
        let failed = std::mem::take(&mut self.applied).undo();
        drop(self.lock.take());
        if failed.is_empty() {
            let _ = fs::remove_dir_all(&self.work);
            return reverted.map_err(|e| format!("The earlier game build could not be put back ({e}): press Set up to build it again."));
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

/// Steps 1 to 4 of the header. Err: nothing changed, unless it says otherwise. `report` gets about 100 Progress per phase and step.
pub fn install(t: &Target, p: &Paths, st: &UpdateState, report: &mut dyn FnMut(Progress), steps: &Steps) -> Result<Session, String> {
    while st.cleaning.load(SeqCst) {
        if st.cancel.load(SeqCst) {
            return Err(CANCELLED.into());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let work = p.install.join(UPDATE_DIR);
    reset_work(&p.install, &work)?;
    let lock = File::create(work.join(LOCK)).ok().filter(|f| f.try_lock().is_ok()).ok_or("Another launcher window is updating. Close it first.")?;
    match stages(t, p, st, &work, report, steps) {
        Ok(applied) => Ok(Session { applied, work, tools: p.install.join("tools"), lock: Some(lock) }),
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

fn stages(t: &Target, p: &Paths, st: &UpdateState, work: &Path, report: &mut dyn FnMut(Progress), steps: &Steps) -> Result<Applied, String> {
    let free = crate::preflight::free_bytes(&p.install);
    if !enough_space(free, t.size) {
        let gb = |b: u64| format!("{:.1} GB", b as f64 / crate::preflight::GB as f64);
        return Err(format!("Not enough free disk space on {}: the update needs about {} and {} is free.", crate::preflight::drive_of(&p.install), gb(t.size.saturating_mul(2)), gb(free.unwrap_or(0))));
    }
    let mut gate = Throttle::default();
    let mut tell = |p: Progress| {
        if gate.pass(&p) {
            report(p);
        }
    };
    let mut send = |phase: &str, done: u64, total: u64| tell(Progress::of(phase, "", done, total));
    send("download", 0, t.size);
    let agent = ureq::AgentBuilder::new().timeout_connect(CONNECT_TIMEOUT).timeout_read(READ_TIMEOUT).user_agent(&user_agent(env!("CARGO_PKG_VERSION"))).build();
    let failed = |e: String| format!("The download failed: {}. Nothing was changed. Try again.", e);
    let sums = String::from_utf8_lossy(&get(&agent, &t.sums_url, 1 << 16).map_err(failed)?).into_owned();
    let want = sum_for(&sums, &t.asset).ok_or("This release has no checksum for its download yet. Try again in a while. Nothing was changed.")?;

    let archive = work.join(&t.asset);
    let resp = agent.get(&t.asset_url).call().map_err(|e| failed(e.to_string()))?;
    let save_failed = |e: io::Error| format!("Could not save the update: {}. Nothing was changed.", e);
    let mut out = File::create(&archive).map_err(save_failed)?;
    let say = download::Said { read: &|e| format!("The download failed: {}. Nothing was changed. Try again.", e), write: &|e| format!("Could not save the update: {}. Nothing was changed.", e) };
    let (n, got) = download::copy_checked(&mut resp.into_reader(), &mut out, t.size, &st.cancel, &mut |n| send("download", n, t.size), &say)?.ok_or(CANCELLED)?;
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
    drop(send);
    put_in_place(&package, p, st, work, &mut tell, steps)
}

/// Steps 3 and 4 of the header, from the unpacked package on. Plain paths: the tests run it on a fake install with their own steps.
fn put_in_place(package: &Path, p: &Paths, st: &UpdateState, work: &Path, tell: &mut dyn FnMut(Progress), steps: &Steps) -> Result<Applied, String> {
    if !steps.code.prepare(&package.join("tools"), &st.cancel, &mut |step, done, total| tell(Progress::of("prepare", step, done, total)))? {
        return Err(CANCELLED.into());
    }
    // the last moment to turn back: the game may run (it stayed playable), and Steam can start it without the launcher
    wait_idle(st, steps.busy, tell)?;
    (steps.ready)()?;
    let job = Job { root: &p.install, package, work, launcher_name: PACKAGE_LAUNCHER, launcher: &p.launcher, game_dll: game_dll(p) };
    let applied = apply(&job, &mut |phase, done, total| tell(Progress::of(phase, "", done, total)))?;
    let undone = |e: String, applied: Applied| {
        let failed = applied.undo();
        if failed.is_empty() { format!("{e} Nothing was changed.") } else { format!("{e} {}", rollback_message(&failed)) }
    };
    // macOS: the package carries the runtime dylib twice (beside the launcher and in tools/build/sdk), so the player's copy
    // follows the one the update replaced. Nothing is signed: the release zip is unsigned by design.
    #[cfg(target_os = "macos")]
    if let Err(e) = sync_runtime_lib(&p.install) {
        return Err(undone(e, applied));
    }
    tell(Progress::of("adopt", "", 0, 1));
    if let Err(e) = steps.code.adopt(&p.install.join("tools")) {
        return Err(undone(format!("The new game build could not be put in place: {e}"), applied));
    }
    tell(Progress::of("adopt", "", 1, 1));
    Ok(applied)
}

/// Waits until nothing keeps the install busy (the game is closed), then marks the swap as begun. Cancel still stops it here.
/// The look and the mark are taken under the gate Play holds until its game has started, so no game starts in between.
fn wait_idle(st: &UpdateState, busy: &dyn Fn() -> Option<String>, tell: &mut dyn FnMut(Progress)) -> Result<(), String> {
    loop {
        if st.cancel.load(SeqCst) {
            return Err(CANCELLED.into());
        }
        {
            let _gate = st.hold_swap();
            if busy().is_none() {
                st.applying.store(true, SeqCst);
                return Ok(());
            }
        }
        tell(Progress::of("wait", "", 0, 1));
        std::thread::sleep(WAIT_POLL);
    }
}

/// The package carries the runtime dylib twice: beside the launcher, and in tools/build/sdk, which is where Set up copies it
/// from into the game folder. Bring the copies already on the player's disk along with the one the update replaced (an install
/// made from an older bundle has the second one in Contents/Frameworks).
#[cfg_attr(not(target_os = "macos"), allow(dead_code))] // type-checked on every host
fn sync_runtime_lib(root: &Path) -> Result<(), String> {
    let Some(parent) = root.parent() else { return Ok(()) };
    let src = root.join("tools").join("build").join("sdk").join("libpkp_rt.dylib");
    if !src.is_file() {
        return Ok(());
    }
    for dst in [root.join("libpkp_rt.dylib"), parent.join("Frameworks").join("libpkp_rt.dylib")] {
        if dst.is_file() {
            fs::copy(&src, &dst).map_err(|e| format!("Could not update the runtime library in {} ({e}).", dst.display()))?;
        }
    }
    Ok(())
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
mod tests {
    use super::*;

    #[test]
    fn the_runtime_library_follows_the_one_the_update_replaced() {
        // both copies the package ships: beside the launcher, and the one in Contents/Frameworks an older bundle install has
        let tmp = std::env::temp_dir().join(format!("nxrt-runtime-lib-{}", std::process::id()));
        let root = tmp.join("PokopiaRecomp-0.12.0");
        let frameworks = tmp.join("Frameworks");
        fs::create_dir_all(root.join("tools").join("build").join("sdk")).unwrap();
        fs::create_dir_all(&frameworks).unwrap();
        let (new, beside, in_frameworks) =
            (root.join("tools/build/sdk/libpkp_rt.dylib"), root.join("libpkp_rt.dylib"), frameworks.join("libpkp_rt.dylib"));
        fs::write(&new, b"new").unwrap();
        fs::write(&beside, b"old").unwrap();
        fs::write(&in_frameworks, b"old").unwrap();
        assert!(sync_runtime_lib(&root).is_ok());
        assert_eq!(fs::read(&beside).unwrap(), b"new", "the copy beside the launcher is the updated one");
        assert_eq!(fs::read(&in_frameworks).unwrap(), b"new", "and so is an older bundle install's copy");
        let _ = fs::remove_dir_all(&tmp);
    }

    // ---- the order around the swap: prepare (install untouched), wait for the game, swap, adopt; and every way back ----------------

    use std::cell::{Cell, RefCell};

    /// A GameCode that records what was asked of it and checks what the install looked like at that moment.
    struct Fake {
        root: PathBuf,
        prepare: Result<bool, String>,
        adopt: Result<(), String>,
        calls: RefCell<Vec<&'static str>>,
    }

    impl Fake {
        fn new(root: &Path) -> Fake {
            Fake { root: root.to_path_buf(), prepare: Ok(true), adopt: Ok(()), calls: RefCell::new(Vec::new()) }
        }
        fn tools_now(&self) -> String {
            fs::read_to_string(self.root.join("tools/build/pkp_build.py")).unwrap()
        }
    }

    impl GameCode for Fake {
        fn prepare(&self, tools: &Path, _cancel: &AtomicBool, report: &mut dyn FnMut(&str, u64, u64)) -> Result<bool, String> {
            assert_eq!(fs::read_to_string(tools.join("build/pkp_build.py")).unwrap(), "new tools", "prepare runs the downloaded package's tools");
            assert_eq!(self.tools_now(), "old tools", "nothing in the install has changed while the game is prepared");
            report("compile", 16, 284);
            self.calls.borrow_mut().push("prepare");
            self.prepare.clone()
        }
        fn adopt(&self, tools: &Path) -> Result<(), String> {
            assert_eq!(fs::read_to_string(tools.join("build/pkp_build.py")).unwrap(), "new tools", "adopt runs after the swap, with the new tools");
            self.calls.borrow_mut().push("adopt");
            self.adopt.clone()
        }
        fn revert(&self, tools: &Path) -> Result<(), String> {
            assert_eq!(fs::read_to_string(tools.join("build/pkp_build.py")).unwrap(), "new tools", "revert runs before the swap is undone");
            self.calls.borrow_mut().push("revert");
            Ok(())
        }
        fn finish(&self) {
            self.calls.borrow_mut().push("finish");
        }
    }

    /// An install with an old launcher and tools, and an unpacked package with new ones: (temp root, paths, package, work).
    fn fake_install(name: &str) -> (PathBuf, Paths, PathBuf, PathBuf) {
        let tmp = std::env::temp_dir().join(format!("nxrt-update-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&tmp);
        let install = tmp.join("install");
        let work = install.join(UPDATE_DIR);
        let package = work.join("staged").join("PokopiaRecomp-1.0.0");
        for (root, what) in [(&install, "old"), (&package, "new")] {
            fs::create_dir_all(root.join("tools/build")).unwrap();
            fs::write(root.join("tools/build/pkp_build.py"), format!("{what} tools")).unwrap();
            fs::write(root.join(PACKAGE_LAUNCHER), format!("{what} launcher")).unwrap();
        }
        let paths = Paths { launcher: install.join(PACKAGE_LAUNCHER), game_dir: install.clone(), install };
        (tmp, paths, package, work)
    }

    fn untouched(p: &Paths) -> bool {
        fs::read_to_string(p.install.join("tools/build/pkp_build.py")).unwrap() == "old tools" && fs::read_to_string(&p.launcher).unwrap() == "old launcher"
    }

    /// put_in_place with `busy` and the fake; returns its result and the phases it reported, in order (repeats folded).
    fn run(p: &Paths, package: &Path, work: &Path, st: &UpdateState, busy: &dyn Fn() -> Option<String>, code: &Fake) -> (Result<Applied, String>, Vec<String>) {
        let mut phases: Vec<String> = Vec::new();
        let ready = || Ok(());
        let r = put_in_place(package, p, st, work, &mut |pr: Progress| {
            if phases.last() != Some(&pr.phase) {
                phases.push(pr.phase.clone());
            }
            if pr.phase == "prepare" {
                assert_eq!((pr.step.as_str(), pr.done, pr.total), ("compile", 16, 284), "the game build's own counts reach the page");
            }
        }, &Steps { busy, ready: &ready, code });
        (r, phases)
    }

    #[test]
    fn the_game_is_prepared_before_any_file_moves_and_adopted_right_after_the_swap() {
        let (tmp, p, package, work) = fake_install("order");
        let (st, code) = (UpdateState::default(), Fake::new(&p.install));
        let (r, phases) = run(&p, &package, &work, &st, &|| None, &code);
        assert!(r.is_ok(), "{:?}", r.err());
        assert_eq!(phases, ["prepare", "apply", "adopt"]);
        assert_eq!(*code.calls.borrow(), ["prepare", "adopt"]);
        assert!(st.applying(), "the swap has begun: Play is refused from here on");
        assert_eq!(fs::read_to_string(&p.launcher).unwrap(), "new launcher");
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_cancelled_prepare_leaves_the_install_as_it_was() {
        let (tmp, p, package, work) = fake_install("cancel");
        let (st, mut code) = (UpdateState::default(), Fake::new(&p.install));
        code.prepare = Ok(false);
        let (r, phases) = run(&p, &package, &work, &st, &|| None, &code);
        assert_eq!(r.err().as_deref(), Some(CANCELLED));
        assert_eq!(phases, ["prepare"]);
        assert!(untouched(&p) && !st.applying());
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_failed_prepare_stops_the_update_with_its_error_and_changes_nothing() {
        let (tmp, p, package, work) = fake_install("fail");
        let (st, mut code) = (UpdateState::default(), Fake::new(&p.install));
        code.prepare = Err("Compiling main/c_12.cpp failed".into());
        let (r, _) = run(&p, &package, &work, &st, &|| None, &code);
        assert!(r.err().unwrap().contains("c_12.cpp"));
        assert!(untouched(&p) && !st.applying());
        assert_eq!(*code.calls.borrow(), ["prepare"], "no adopt without a prepared build");
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn the_swap_waits_while_the_game_runs_and_cancel_still_stops_it_there() {
        let (tmp, p, package, work) = fake_install("wait");
        let (st, code) = (UpdateState::default(), Fake::new(&p.install));
        let looks = Cell::new(0);
        let game_for_two_looks = || {
            looks.set(looks.get() + 1);
            assert!(untouched(&p), "nothing moves while the game runs");
            (looks.get() <= 2).then(|| "The game is running.".to_string())
        };
        let (r, phases) = run(&p, &package, &work, &st, &game_for_two_looks, &code);
        assert!(r.is_ok());
        assert_eq!(phases, ["prepare", "wait", "apply", "adopt"]);
        let _ = fs::remove_dir_all(&tmp);

        let (tmp, p, package, work) = fake_install("wait-cancel");
        let (st, code) = (UpdateState::default(), Fake::new(&p.install));
        let player_cancels = || {
            st.cancel();
            Some("The game is running.".to_string())
        };
        let (r, phases) = run(&p, &package, &work, &st, &player_cancels, &code);
        assert_eq!(r.err().as_deref(), Some(CANCELLED));
        assert_eq!(phases, ["prepare", "wait"]);
        assert!(untouched(&p) && !st.applying());
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_failed_adopt_undoes_the_swap() {
        let (tmp, p, package, work) = fake_install("adopt");
        let (st, mut code) = (UpdateState::default(), Fake::new(&p.install));
        code.adopt = Err("pkp.exe is in use".into());
        let (r, phases) = run(&p, &package, &work, &st, &|| None, &code);
        let e = r.err().unwrap();
        assert!(e.contains("pkp.exe is in use") && e.contains("Nothing was changed"), "{e}");
        assert_eq!(phases, ["prepare", "apply", "adopt"]);
        assert!(untouched(&p), "the old launcher and tools are back");
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_launcher_that_does_not_start_gets_the_old_game_code_back_before_its_tools() {
        let (tmp, p, package, work) = fake_install("rollback");
        let (st, code) = (UpdateState::default(), Fake::new(&p.install));
        let applied = run(&p, &package, &work, &st, &|| None, &code).0.unwrap();
        let session = Session { applied, work: work.clone(), tools: p.install.join("tools"), lock: None };
        assert!(session.rollback(&code).is_ok());
        assert_eq!(*code.calls.borrow(), ["prepare", "adopt", "revert"]);
        assert!(untouched(&p));
        let _ = fs::remove_dir_all(&tmp);

        let (tmp, p, package, work) = fake_install("finish");
        let (st, code) = (UpdateState::default(), Fake::new(&p.install));
        let applied = run(&p, &package, &work, &st, &|| None, &code).0.unwrap();
        Session { applied, work: work.clone(), tools: p.install.join("tools"), lock: None }.finish(&code);
        assert_eq!(*code.calls.borrow(), ["prepare", "adopt", "finish"]);
        assert!(!work.exists(), "the backup goes once the new launcher runs");
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn play_holding_the_swap_keeps_it_from_beginning() {
        let st = std::sync::Arc::new(UpdateState::default());
        let held = std::sync::Arc::new(std::sync::Barrier::new(2));
        let (st2, held2) = (st.clone(), held.clone());
        let play = std::thread::spawn(move || {
            let _gate = st2.hold_swap();
            held2.wait();
            std::thread::sleep(Duration::from_millis(300));
            assert!(!st2.applying(), "no swap begins while Play holds it");
        });
        held.wait();
        let t = std::time::Instant::now();
        wait_idle(&st, &|| None, &mut |_| {}).unwrap();
        assert!(t.elapsed() >= Duration::from_millis(250) && st.applying());
        play.join().unwrap();
    }
}
