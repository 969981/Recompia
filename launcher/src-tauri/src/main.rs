// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod dlss;
mod dump;
mod gamebuild;
mod ini;
mod nup;
mod play;
mod settings;
mod shaders;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    dump_dir: String,
    /// Folder with pkp.exe. Empty: the folder the launcher is in.
    game_dir: String,
    minimize_on_play: bool,
    /// Play waits for every shader of the corpus (the old setup), not just the used ones (shaders.rs, Part::Used)
    full_shader_setup: bool,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

fn game_dir_of(cfg: &Config) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        exe_dir()
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

#[derive(Serialize)]
struct Overview {
    game_dir: String,
    default_game_dir: String,
    exe_exists: bool,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(&app)?, &text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg);
    Overview {
        game_dir: dump::fwd(&dir),
        default_game_dir: dump::fwd(&exe_dir()),
        exe_exists: dir.join("pkp.exe").is_file(),
    }
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
async fn pick_nup(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your Pokopia .nup package").add_filter("NUP package", &["nup"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[derive(Serialize, Clone)]
struct NupProgress {
    done: u64,
    total: u64,
    file: String,
}

/// Unpacks a .nup into `<game folder>/nup_dump` (base/ and updates/<version>/, the same layout as a dump folder) and
/// returns that folder. Emits "nup-progress". A finished unpack of the same package is reused.
#[tauri::command]
async fn import_nup(app: AppHandle, state: State<'_, Arc<SetupState>>, path: String) -> Result<String, String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let out = game_dir_of(&load_config(&app)).join("nup_dump");
    let src = PathBuf::from(path.trim());
    let emitter = app.clone();
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        nup::extract(
            &src,
            &out,
            &mut |done, total, file| {
                // the UI only needs ~100 updates over the whole unpack
                if total == 0 || done == total || done - last >= total / 100 {
                    last = done;
                    let _ = emitter.emit("nup-progress", NupProgress { done, total, file: file.to_string() });
                }
            },
            &|| false,
        )
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(dump::fwd(&res))
}

#[tauri::command]
fn validate_dump(path: String) -> dump::DumpReport {
    dump::scan(&path)
}

/// Validates, stores the dump folder, and points `[fs] romfs` in pkp.ini at the chosen romfs.
#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let report = dump::scan(&path);
    let chosen = report.chosen.clone().ok_or_else(|| report.errors.join(" "))?;
    let mut cfg = load_config(&app);
    cfg.dump_dir = report.path.clone();
    save_config(app.clone(), cfg.clone())?;
    settings::apply(
        &game_dir_of(&cfg),
        &[
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "exefs".into(), value: settings::portable(&game_dir_of(&cfg), &chosen.exefs) },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs".into(), value: settings::portable(&game_dir_of(&cfg), &chosen.romfs) },
        ],
    )?;
    Ok(report)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> settings::SettingsDump {
    settings::read(&game_dir_of(&load_config(&app)))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir_of(&load_config(&app)), &edits)
}

#[tauri::command]
fn play(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    if cfg.dump_dir.trim().is_empty() {
        return Err("Choose your game files first (Setup).".into());
    }
    // a .nup package is played as it is: pkp reads exefs and romfs straight from it
    let (exefs, romfs) = if cfg.dump_dir.trim().ends_with(".nup") && Path::new(cfg.dump_dir.trim()).is_file() {
        (cfg.dump_dir.trim().to_string(), cfg.dump_dir.trim().to_string())
    } else {
        let report = dump::scan(&cfg.dump_dir);
        let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
        (chosen.exefs, chosen.romfs)
    };
    let dir = game_dir_of(&cfg);
    let tools_dir = exe_dir().join("tools");
    if gamebuild::is_package_b(&tools_dir) {
        match gamebuild::built_state(&dir, &tools_dir) {
            gamebuild::Built::Current => {}
            gamebuild::Built::Stale => return Err(gamebuild::REBUILD_MESSAGE.into()),
            gamebuild::Built::No => return Err("Build the game first (Game files, Set up).".into()),
        }
    }
    if app.state::<Arc<SetupState>>().progress.lock().unwrap().state == "running" {
        return Err("Setup is still running (Game files).".into());
    }
    if shaders::is_stale(&dir, &tools_dir) {
        return Err("This release has a newer shader translator: set up again to translate the shaders again (Game files, Set up).".into());
    }
    let ready = if cfg.full_shader_setup { shaders::is_complete(&dir, &tools_dir) } else { shaders::is_playable(&dir, &tools_dir) };
    if !ready {
        return Err("Finish the shader setup first (Game files, Set up).".into());
    }
    settings::ensure_play_paths(&dir, &exefs, &romfs)?;
    play::start(&app, &dir, cfg.minimize_on_play)
}

/// Shader setup: the running job's progress and its cancel flag.
#[derive(Default)]
struct SetupState {
    progress: Mutex<shaders::Progress>,
    cancel: AtomicBool,
}

#[derive(Serialize)]
struct SetupStatus {
    progress: shaders::Progress,
    /// setup.json says finished and the cache is there
    complete: bool,
    /// an earlier setup started (Set up continues it)
    started: bool,
    /// pkp.exe is from an older release whose game code this one changes: Set up rebuilds it
    rebuild: bool,
    /// the shader cache was made by another translator version than this release's: Set up translates it again
    retranslate: bool,
    /// Play may start: complete, or the used shaders are done (and the launcher option does not ask for all of them)
    playable: bool,
    /// the used shaders are done, the rest is not: the background translation has work
    rest_pending: bool,
    /// the background translation's last progress ("idle" before its first run in this session)
    background: shaders::Progress,
    /// the user paused the background translation (until Resume or the next launch)
    background_paused: bool,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<SetupState>>) -> SetupStatus {
    let dir = game_dir_of(&load_config(&app));
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let tools_dir = exe_dir().join("tools");
    let state = if gamebuild::is_package_b(&tools_dir) { gamebuild::built_state(&dir, &tools_dir) } else { gamebuild::Built::Current };
    let started = !shaders::read_marker(&dir).romfs.is_empty() || dir.join("build").join("state.json").is_file();
    let full = load_config(&app).full_shader_setup;
    let bg = app.state::<Arc<shaders::Background>>();
    let mut background = bg.progress.lock().unwrap().clone();
    if background.state.is_empty() {
        background.state = "idle".into();
    }
    SetupStatus {
        progress,
        complete: state == gamebuild::Built::Current && shaders::is_complete(&dir, &tools_dir),
        started,
        rebuild: state == gamebuild::Built::Stale,
        retranslate: shaders::is_stale(&dir, &tools_dir),
        playable: state == gamebuild::Built::Current
            && if full { shaders::is_complete(&dir, &tools_dir) } else { shaders::is_playable(&dir, &tools_dir) },
        rest_pending: shaders::rest_pending(&dir),
        background,
        background_paused: bg.paused(),
    }
}

/// First-run setup: (a) the pkp.ini paths, (b) in a model (b) release, pkp.exe built from the player's dump
/// (gamebuild.rs), (c) the shader cache in shader_cache/spirv, on a worker thread with "setup-status" events.
#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<SetupState>>) -> Result<(), String> {
    let cfg = load_config(&app);
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = game_dir_of(&cfg);
    let tools_dir = exe_dir().join("tools");
    let package_b = gamebuild::is_package_b(&tools_dir);
    if !package_b && !dir.join("pkp.exe").is_file() {
        return Err(format!("pkp.exe was not found in {}. Put the launcher next to pkp.exe or set the game folder in Settings.", dump::fwd(&dir)));
    }
    {
        let mut p = state.progress.lock().unwrap();
        if p.state == "running" {
            return Err("Setup is already running.".into());
        }
        *p = shaders::Progress { state: "running".into(), phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
    }
    settings::ensure_play_paths(&dir, &chosen.exefs, &chosen.romfs)?;
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let jobs = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1);
    let build_job = gamebuild::Job { game_dir: dir.clone(), exefs: PathBuf::from(&chosen.exefs), jobs: jobs + 1 };
    // the used shaders only (Play turns on after them, the rest follows in the background), unless the option asks for all
    let part = if cfg.full_shader_setup { shaders::Part::All } else { shaders::Part::Used };
    let job = shaders::Job { game_dir: dir, romfs: PathBuf::from(&chosen.romfs), main: PathBuf::from(&chosen.exefs).join("main"), jobs, limit: 0, part };
    // Set up and the background translation never run at once (both write the cache)
    let hold = app.state::<Arc<shaders::Background>>().inner().hold();
    std::thread::spawn(move || {
        let emit = |p: shaders::Progress| {
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        let result = (|| {
            // no build yet, or one from an older release (stale tools stamp): build pkp.exe (again)
            if package_b && !gamebuild::is_built(&build_job.game_dir, &tools_dir) {
                let tools = gamebuild::find_tools(&tools_dir)?;
                let built = gamebuild::run(&build_job, &tools, &st.cancel, |t| {
                    emit(shaders::Progress { state: "running".into(), phase: t.phase, done: t.done, total: t.total, message: t.message })
                })?;
                if !built {
                    return Ok(false);
                }
            }
            let tools = shaders::find_tools(&tools_dir)?;
            // a finished cache of the same romfs has nothing left to translate (re-extracting would only find that out)
            let same = shaders::read_marker(&job.game_dir).romfs == job.romfs.to_string_lossy().replace('\\', "/");
            let done = same
                && if job.part == shaders::Part::All { shaders::is_complete(&job.game_dir, &tools_dir) } else { shaders::is_playable(&job.game_dir, &tools_dir) };
            if !done && !shaders::run(&job, &tools, &st.cancel, |p| emit(p))? {
                return Ok(false);
            }
            // the shaders the game builds while it runs: one hidden run collects those of the opening; later ones are
            // collected by every normal run, and every Play checks them all (play.rs)
            emit(shaders::Progress {
                state: "running".into(),
                phase: "warmup".into(),
                message: format!("Running the game once in the background to collect the shaders it builds while it runs (about {} s)...", shaders::WARMUP_SECONDS),
                ..Default::default()
            });
            Ok(shaders::prepare(&job.game_dir, &tools_dir, &st.cancel)?.is_some())
        })();
        let end = match result {
            Ok(true) => shaders::Progress { state: "done".into(), phase: "finish".into(), message: "Setup finished.".into(), ..Default::default() },
            Ok(false) => shaders::Progress { state: "cancelled".into(), message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        };
        emit(end);
        drop(hold);
        start_background(&app);
    });
    Ok(())
}

/// Starts the background translation of the shaders outside the used set (shaders.rs, Part::Rest) when there is work and
/// nothing holds it: after Set up, after the game exits, at launch (a run cut short by closing the launcher continues),
/// and on Resume. Half the cores at idle priority; it stops while the game runs.
pub(crate) fn start_background(app: &AppHandle) {
    let cfg = load_config(app);
    let dir = game_dir_of(&cfg);
    let tools_dir = exe_dir().join("tools");
    if !shaders::rest_pending(&dir) || shaders::is_stale(&dir, &tools_dir) {
        return;
    }
    let Some(chosen) = dump::scan(&cfg.dump_dir).chosen else { return };
    let romfs = PathBuf::from(&chosen.romfs);
    if shaders::read_marker(&dir).romfs != romfs.to_string_lossy().replace('\\', "/") {
        return;
    }
    let Ok(tools) = shaders::find_tools(&tools_dir) else { return };
    let bg = app.state::<Arc<shaders::Background>>().inner().clone();
    if !bg.begin() {
        return;
    }
    let jobs = std::thread::available_parallelism().map(|n| n.get() / 2).unwrap_or(1).max(1);
    let job = shaders::Job { game_dir: dir, romfs, main: PathBuf::from(&chosen.exefs).join("main"), jobs, limit: 0, part: shaders::Part::Rest };
    let app = app.clone();
    std::thread::spawn(move || {
        let emit = |p: shaders::Progress| {
            *bg.progress.lock().unwrap() = p.clone();
            let _ = app.emit("background-status", p);
        };
        let result = shaders::run(&job, &tools, &bg.stop, |p| emit(p));
        bg.end();
        emit(match result {
            Ok(true) => shaders::Progress { state: "done".into(), phase: "finish".into(), message: "All shaders are translated.".into(), ..Default::default() },
            Ok(false) => {
                let why = if bg.paused() { "Paused. Resume, or it continues at the next launch." } else { "Paused while the game runs; it continues afterwards." };
                shaders::Progress { state: "cancelled".into(), message: why.into(), ..Default::default() }
            }
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        });
    });
}

#[tauri::command]
fn pause_background(state: State<Arc<shaders::Background>>) {
    state.set_paused(true);
}

#[tauri::command]
fn resume_background(app: AppHandle, state: State<Arc<shaders::Background>>) {
    state.set_paused(false);
    start_background(&app);
}

#[tauri::command]
fn cancel_setup(state: State<Arc<SetupState>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

#[tauri::command]
fn dlss_status(app: AppHandle) -> dlss::Status {
    dlss::status(&game_dir_of(&load_config(&app)), &exe_dir().join("tools"))
}

/// Downloads NVIDIA's nvngx_dlss.dll (see dlss.rs) on a worker thread, with "dlss-progress" events.
#[tauri::command]
async fn fetch_dlss(app: AppHandle) -> Result<(), String> {
    let dir = game_dir_of(&load_config(&app));
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        dlss::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !play::REPORT_FILES.contains(&name.as_str()) {
        return Err("That file cannot be opened from here.".into());
    }
    let path = game_dir_of(&load_config(&app)).join(&name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    play::open_with_shell(&game_dir_of(&load_config(&app)))
}


/// Test runs only (PKP_LAUNCHER_EVAL_FILE set): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var("PKP_LAUNCHER_EVAL_FILE").map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(SetupState::default()))
        .manage(Arc::new(shaders::Background::default()))
        .setup(|app| {
            // the background translation a closed launcher left unfinished continues
            let handle = app.handle().clone();
            std::thread::spawn(move || start_background(&handle));
            // Test runs drive the page without input: PKP_LAUNCHER_EVAL_FILE=<path>. A JS file written there is run in the page once and
            // deleted; it reports back through document.title (the window title). WebView2's DevTools port does not open for Tauri apps here.
            if let Ok(path) = std::env::var("PKP_LAUNCHER_EVAL_FILE") {
                if let Some(win) = app.get_webview_window("main") {
                    std::thread::spawn(move || loop {
                        std::thread::sleep(std::time::Duration::from_millis(300));
                        if let Ok(js) = std::fs::read_to_string(&path) {
                            let _ = std::fs::remove_file(&path);
                            let _ = win.eval(&js);
                        }
                    });
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            test_report,
            get_config,
            save_config,
            get_overview,
            pick_folder,
            validate_dump,
            pick_nup,
            import_nup,
            save_dump,
            read_settings,
            write_settings,
            play,
            setup_status,
            start_setup,
            cancel_setup,
            pause_background,
            resume_background,
            dlss_status,
            fetch_dlss,
            game_status,
            open_game_file,
            open_game_dir,
        ])
        .build(tauri::generate_context!())
        .expect("error while running the launcher")
        .run(|app, event| {
            if let tauri::RunEvent::Exit = event {
                // a harness is a separate process and would go on writing the cache after the launcher is gone (racing
                // the next launch): stop the running tools and give them a moment to be killed
                let bg = app.state::<Arc<shaders::Background>>().inner().clone();
                let _hold = bg.hold();
                let setup = app.state::<Arc<SetupState>>().inner().clone();
                setup.cancel.store(true, Ordering::SeqCst);
                for _ in 0..20 {
                    if !bg.running() && setup.progress.lock().unwrap().state != "running" {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(100));
                }
            }
        });
}
