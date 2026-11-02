// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod dump;
mod ini;
mod play;
mod settings;
mod shaders;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
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
    fs::write(config_path(&app)?, text).map_err(|e| format!("cannot save launcher settings: {}", e))
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
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "exefs".into(), value: chosen.exefs },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs".into(), value: chosen.romfs },
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
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
    let dir = game_dir_of(&cfg);
    if !shaders::is_complete(&dir) {
        return Err("Finish the shader setup first (Game files, Set up).".into());
    }
    settings::ensure_play_paths(&dir, &chosen.exefs, &chosen.romfs)?;
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
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<SetupState>>) -> SetupStatus {
    let dir = game_dir_of(&load_config(&app));
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    SetupStatus { progress, complete: shaders::is_complete(&dir), started: !shaders::read_marker(&dir).romfs.is_empty() }
}

/// First-run setup: (a) the pkp.ini paths, (b) the shader cache in shader_cache/spirv, on a worker thread with
/// "setup-status" events.
#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<SetupState>>) -> Result<(), String> {
    let cfg = load_config(&app);
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = game_dir_of(&cfg);
    if !dir.join("pkp.exe").is_file() {
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
    let job = shaders::Job { game_dir: dir, romfs: PathBuf::from(&chosen.romfs), main: PathBuf::from(&chosen.exefs).join("main"), jobs, limit: 0 };
    std::thread::spawn(move || {
        let emit = |p: shaders::Progress| {
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        let result = shaders::find_tools(&exe_dir().join("tools")).and_then(|tools| shaders::run(&job, &tools, &st.cancel, |p| emit(p)));
        let end = match result {
            Ok(true) => shaders::Progress { state: "done".into(), phase: "finish".into(), message: "Setup finished.".into(), ..Default::default() },
            Ok(false) => shaders::Progress { state: "cancelled".into(), message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        };
        emit(end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(state: State<Arc<SetupState>>) {
    state.cancel.store(true, Ordering::SeqCst);
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

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(SetupState::default()))
        .invoke_handler(tauri::generate_handler![
            get_config,
            save_config,
            get_overview,
            pick_folder,
            validate_dump,
            save_dump,
            read_settings,
            write_settings,
            play,
            setup_status,
            start_setup,
            cancel_setup,
            game_status,
            open_game_file,
            open_game_dir,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
