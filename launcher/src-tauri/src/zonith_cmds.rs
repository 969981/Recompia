// SPDX-License-Identifier: GPL-3.0-or-later
//! Zonith's commands: its releases, downloading its setup app (and replacing an older one), and starting it. Zonith's setup app does
//! the rest (setting the game up from the player's files, and playing it). The pure part is zonith.rs.
//!
//! `<app local data>/zonith/`: `<tag>/` holds a release's setup (Windows: the zip and its unpacked TotK-Setup-windows/, Linux: the
//! AppImage, which also stays because an install records `$APPIMAGE --rebuild` to rebuild the game), `current.json` names the one in
//! use, `releases.json` is the last release list (shown offline).
//! Events: `zonith-progress` (zonith::ZonithProgress) while a download runs, `zonith-game` { state: running | exited, code }.

use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering::SeqCst};
use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager};

use crate::config::load_config;
use crate::states::States;
use crate::zonith::{self, Current, Latest, ReleaseRow, ZonithProgress};

const CANCELLED: &str = "Zonith download cancelled.";
const LIST_LIMIT: u64 = 4 << 20;

#[derive(Default)]
struct Shared {
    busy: AtomicBool,
    cancel: AtomicBool,
    progress: Mutex<Option<ZonithProgress>>,
    game: AtomicBool,
    /// the last release list (None until asked) and why it could not be fetched
    releases: Mutex<Option<(Vec<zonith::GhRelease>, Option<String>)>>,
}

static S: LazyLock<Shared> = LazyLock::new(Shared::default);

/// A Zonith download runs (the launcher's own update waits: its restart would cut it off).
pub fn job_running() -> bool {
    S.busy.load(SeqCst)
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct InstalledSetup {
    pub tag: String,
    pub path: String,
}

#[derive(Serialize)]
pub struct ZonithStatus {
    pub supported: bool,
    pub releases: Vec<ReleaseRow>,
    pub latest: Option<Latest>,
    pub release_error: Option<String>,
    pub installed: Option<InstalledSetup>,
    pub update: bool,
    pub job: Option<ZonithProgress>,
    pub running: bool,
}

#[derive(Serialize, Clone)]
struct GameEvent {
    state: &'static str,
    code: Option<i32>,
}

fn zonith_dir(app: &AppHandle) -> Result<PathBuf, String> {
    Ok(app.path().app_local_data_dir().map_err(|e| e.to_string())?.join("zonith"))
}

fn fwd(p: &Path) -> String {
    crate::dump::fwd(p)
}

/// One GET of the release list. 404: the repository is not public yet.
fn fetch(app: &AppHandle) -> Result<Vec<u8>, String> {
    use std::io::Read;
    let agent = ureq::AgentBuilder::new()
        .timeout(Duration::from_secs(15))
        .user_agent(&crate::releases::user_agent(&app.package_info().version.to_string()))
        .build();
    let url = crate::testhook::zonith_releases_url().unwrap_or_else(|| zonith::RELEASES_URL.into());
    match agent.get(&url).set("Accept", "application/vnd.github+json").call() {
        Ok(r) => {
            let mut body = Vec::new();
            r.into_reader().take(LIST_LIMIT).read_to_end(&mut body).map_err(|_| "offline".to_string())?;
            Ok(body)
        }
        Err(ureq::Error::Status(404, _)) => Err("not_public".into()),
        Err(e) => {
            eprintln!("zonith: release check failed: {e}");
            Err("offline".into())
        }
    }
}

/// The releases: this session's copy unless `refresh`, else one GET, then the cache file on failure. The lock is held over the GET.
fn releases(app: &AppHandle, refresh: bool) -> (Vec<zonith::GhRelease>, Option<String>) {
    let mut mem = S.releases.lock().unwrap();
    if let (Some(v), false) = (mem.as_ref(), refresh) {
        return v.clone();
    }
    let cache = zonith_dir(app).map(|d| d.join("releases.json")).ok();
    let got = match fetch(app).and_then(|b| zonith::parse_releases(&b).map(|l| (b, l)).map_err(|_| "offline".to_string())) {
        Ok((body, list)) => {
            if let Some(c) = &cache {
                let _ = std::fs::create_dir_all(c.parent().unwrap());
                if let Err(e) = crate::settings::write_atomic(c, &String::from_utf8_lossy(&body)) {
                    eprintln!("zonith: release list not cached: {e}");
                }
            }
            (list, None)
        }
        // not public: nothing to show, whatever an older cache says
        Err(e) if e == "not_public" => (Vec::new(), Some(e)),
        Err(e) => {
            let cached = cache.and_then(|c| std::fs::read(c).ok()).and_then(|b| zonith::parse_releases(&b).ok());
            (cached.or_else(|| mem.take().map(|v| v.0)).unwrap_or_default(), Some(e))
        }
    };
    *mem = Some(got.clone());
    got
}

#[tauri::command]
pub async fn zonith_status(app: AppHandle, refresh: Option<bool>) -> ZonithStatus {
    let refresh = refresh.unwrap_or(false);
    let a = app.clone();
    tauri::async_runtime::spawn_blocking(move || status(&a, refresh)).await.unwrap_or_else(|_| ZonithStatus {
        supported: zonith::SUPPORTED,
        releases: Vec::new(),
        latest: None,
        release_error: Some("offline".into()),
        installed: None,
        update: false,
        job: None,
        running: false,
    })
}

fn installed(app: &AppHandle) -> Option<(Current, PathBuf)> {
    let dir = zonith_dir(app).ok()?;
    let c = zonith::read_current(&dir)?;
    let tag_dir = dir.join(zonith::safe_tag(&c.tag));
    Some((c, tag_dir))
}

fn status(app: &AppHandle, refresh: bool) -> ZonithStatus {
    let (list, release_error) = if zonith::SUPPORTED { releases(app, refresh) } else { (Vec::new(), None) };
    let latest = zonith::latest(&list, zonith::ASSET_SUFFIX);
    let installed = installed(app).map(|(c, d)| InstalledSetup { tag: c.tag, path: fwd(&d) });
    let update = match (&installed, &latest) {
        (Some(i), Some(l)) => zonith::update_available(&i.tag, &l.tag),
        _ => false,
    };
    ZonithStatus {
        supported: zonith::SUPPORTED,
        releases: zonith::rows(&list),
        latest,
        release_error,
        installed,
        update,
        job: if job_running() { S.progress.lock().unwrap().clone() } else { None },
        running: S.game.load(SeqCst),
    }
}

fn emit(app: &AppHandle, p: ZonithProgress) {
    *S.progress.lock().unwrap() = Some(p.clone());
    let _ = app.emit("zonith-progress", p);
}

/// Clears `busy` however the job ends.
struct BusyGuard;
impl Drop for BusyGuard {
    fn drop(&mut self) {
        S.busy.store(false, SeqCst);
    }
}

/// Async: the release lookup below can take up to the agent's timeout, and a plain command would hold the window's thread for it.
#[tauri::command]
pub async fn zonith_download(app: AppHandle) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || start_download(app)).await.map_err(|e| e.to_string())?
}

fn start_download(app: AppHandle) -> Result<(), String> {
    if !zonith::SUPPORTED {
        return Err("Zonith runs on Windows and Linux only.".into());
    }
    if app.updates().busy() {
        return Err("The launcher is updating. Let it finish first.".into());
    }
    if S.busy.swap(true, SeqCst) {
        return Err("Zonith is already downloading.".into());
    }
    let guard = BusyGuard;
    S.cancel.store(false, SeqCst);
    let (list, err) = releases(&app, true);
    let Some(latest) = zonith::latest(&list, zonith::ASSET_SUFFIX) else {
        return Err(match err.as_deref() {
            Some("not_public") => "Zonith has not been released yet.".to_string(),
            Some(_) => "Could not reach GitHub for the Zonith release. Check the internet connection, then try again.".to_string(),
            None => "The latest Zonith release has no setup this launcher can check for this system.".to_string(),
        });
    };
    let root = zonith_dir(&app)?;
    emit(&app, ZonithProgress::download(0, latest.size));
    std::thread::spawn(move || {
        let _guard = guard;
        let end = match run_job(&app, &root, &latest) {
            Ok(()) => ZonithProgress::end("done", None),
            Err(e) if e == CANCELLED => ZonithProgress::end("cancelled", None),
            Err(e) => ZonithProgress::end("error", Some(e)),
        };
        emit(&app, end);
    });
    Ok(())
}

fn run_job(app: &AppHandle, root: &Path, l: &Latest) -> Result<(), String> {
    let tag = zonith::safe_tag(&l.tag);
    let dir = root.join(&tag);
    std::fs::create_dir_all(&dir).map_err(|e| format!("Could not make {}: {}", fwd(&dir), e))?;
    let pinned = crate::download::Pinned { file: &l.asset, url: &l.download_url, size: l.size, sha256: &l.sha256 };
    let mut last = Instant::now() - Duration::from_secs(1);
    let fetched = crate::download::fetch(&crate::download::agent(true), &pinned, &dir, &S.cancel, &mut |n| {
        if last.elapsed() >= Duration::from_millis(250) || n == l.size {
            last = Instant::now();
            emit(app, ZonithProgress::download(n, l.size));
        }
    })?;
    if !fetched {
        return Err(CANCELLED.into());
    }
    emit(app, ZonithProgress::unpack());
    #[cfg(windows)]
    {
        if !dir.join(zonith::WINDOWS_ROOT).join(zonith::WINDOWS_EXE).is_file() {
            let _ = std::fs::remove_dir_all(dir.join("unpacked"));
            crate::update::extract(&dir, &l.asset, "unpacked", &S.cancel)
                .map_err(|e| if e == crate::update::CANCELLED { CANCELLED.to_string() } else { e.replace("the update", "the Zonith setup") })?;
            let _ = std::fs::remove_dir_all(dir.join(zonith::WINDOWS_ROOT));
            std::fs::rename(dir.join("unpacked").join(zonith::WINDOWS_ROOT), dir.join(zonith::WINDOWS_ROOT))
                .map_err(|e| format!("The Zonith setup did not unpack as expected: {e}"))?;
            let _ = std::fs::remove_dir_all(dir.join("unpacked"));
            let _ = std::fs::remove_file(dir.join("tar.log"));
        }
        // the unpacked setup is what runs; the zip would only hold its size again
        let _ = std::fs::remove_file(dir.join(&l.asset));
    }
    let current = Current { tag: l.tag.clone(), asset: l.asset.clone() };
    // Linux: the checked AppImage replaces the one at the folder's top in one rename; a setup still open keeps the file it runs
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dir.join(&l.asset), std::fs::Permissions::from_mode(0o755)).map_err(|e| format!("Could not make the Zonith setup runnable: {e}"))?;
        std::fs::rename(dir.join(&l.asset), zonith::app_path(root, &current)).map_err(|e| format!("Could not put the Zonith setup in place: {e}"))?;
    }
    if !zonith::app_path(root, &current).is_file() {
        return Err("The Zonith setup is incomplete after downloading it. Try again.".into());
    }
    crate::settings::write_atomic(&root.join(zonith::CURRENT_FILE), &serde_json::to_string(&current).map_err(|e| e.to_string())?)
        .map_err(|e| format!("Could not record the Zonith setup: {e}"))?;
    zonith::remove_old(root, &tag);
    Ok(())
}


#[tauri::command]
pub fn zonith_cancel() {
    if job_running() {
        S.cancel.store(true, SeqCst);
    }
}

/// Starts Zonith's setup app, which sets the game up and plays it. TotK-Setup.exe starts its window's Python and exits at once, so
/// "exited" only means the starter ended.
#[tauri::command]
pub fn zonith_play(app: AppHandle) -> Result<(), String> {
    if job_running() {
        return Err("Zonith is downloading. Let it finish first.".into());
    }
    let (c, dir) = installed(&app).ok_or("Zonith is not downloaded yet.")?;
    let exe = zonith::app_path(&zonith_dir(&app)?, &c);
    if S.game.swap(true, SeqCst) {
        return Err("Zonith is already starting.".into());
    }
    let cwd = exe.parent().map(Path::to_path_buf).unwrap_or(dir);
    #[cfg(windows)]
    let mut cmd = Command::new(&exe);
    #[cfg(not(windows))]
    let mut cmd = crate::host::command(&exe);
    let mut child = match cmd.current_dir(&cwd).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn() {
        Ok(c) => c,
        Err(e) => {
            S.game.store(false, SeqCst);
            return Err(format!("Could not start Zonith: {e}"));
        }
    };
    let _ = app.emit("zonith-game", GameEvent { state: "running", code: None });
    // TotK-Setup.exe hands over to its window's Python and exits at once on Windows, so minimising there would only flash the window
    let minimize = cfg!(not(windows)) && load_config(&app).minimize_on_play;
    let window = app.get_webview_window("main");
    if minimize {
        if let Some(w) = &window {
            let _ = w.minimize();
        }
    }
    std::thread::spawn(move || {
        let code = child.wait().ok().and_then(|s| s.code());
        S.game.store(false, SeqCst);
        if minimize {
            if let Some(w) = &window {
                let _ = w.unminimize();
            }
        }
        let _ = app.emit("zonith-game", GameEvent { state: "exited", code });
    });
    Ok(())
}

#[tauri::command]
pub fn zonith_open_dir(app: AppHandle) -> Result<(), String> {
    let dir = match installed(&app) {
        Some((_, d)) => d,
        None => zonith_dir(&app)?,
    };
    if !dir.is_dir() {
        return Err("Zonith is not downloaded yet.".into());
    }
    crate::play::open_with_shell(&dir)
}
