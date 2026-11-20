// SPDX-License-Identifier: GPL-3.0-or-later
//! Starts pkp.exe, watches it, and reports how it ended.

use serde::Serialize;
use std::path::Path;
use std::sync::Mutex;
use std::time::{Duration, SystemTime};
use tauri::{AppHandle, Emitter, Manager};

use crate::config::Config;
use crate::states::States;

/// Files the launcher may open for the user, by name, inside the game folder.
pub const REPORT_FILES: [&str; 4] = ["pkp_crash.txt", "pkp_freeze.txt", "pkp_fatal.txt", "pkp.log"];

#[derive(Serialize, Clone, Default)]
pub struct ReportFile {
    pub name: String,
    /// Written since this run started.
    pub fresh: bool,
}

/// What the game is doing. The page gets the lowercase word: "idle", "running" or "exited".
#[derive(Serialize, Clone, Copy, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
pub enum GameState {
    Idle,
    Running,
    Exited,
}

#[derive(Serialize, Clone)]
pub struct Status {
    pub state: GameState,
    pub code: Option<i32>,
    pub crashed: bool,
    pub files: Vec<ReportFile>,
    pub message: String,
}

impl Default for Status {
    fn default() -> Self {
        Status { state: GameState::Idle, code: None, crashed: false, files: vec![], message: String::new() }
    }
}

#[derive(Default)]
pub struct PlayState(pub Mutex<Status>);

fn set_status(app: &AppHandle, s: Status) {
    *app.play().0.lock().unwrap() = s.clone();
    let _ = app.emit("game-status", s);
}

fn report_files(dir: &Path, started: SystemTime) -> Vec<ReportFile> {
    REPORT_FILES
        .iter()
        .filter_map(|n| {
            let md = std::fs::metadata(dir.join(n)).ok()?;
            let fresh = md.modified().map(|m| m + Duration::from_secs(2) >= started).unwrap_or(false);
            Some(ReportFile { name: n.to_string(), fresh })
        })
        .collect()
}

/// `coop`: a co-op start (coop_launch.rs); None is a singleplayer start.
pub fn start(app: &AppHandle, game_dir: &Path, cfg: &Config, without_mods: bool, coop: Option<crate::coop_launch::Launch>) -> Result<(), String> {
    let (minimize, discord) = (cfg.minimize_on_play, cfg.discord_presence);
    let gentle = cfg.gentle_build;
    let cloud_islands = cfg.cloud_islands;
    {
        let st = app.play();
        if st.0.lock().unwrap().state == GameState::Running {
            return Err("The game is already running.".into());
        }
    }
    let exe = crate::host::game_exe(game_dir);
    if !exe.is_file() {
        return Err(format!(
            "{} was not found in {}. Put the launcher next to it or set the game folder in Settings.",
            crate::host::game_exe_name(),
            crate::dump::fwd(game_dir)
        ));
    }
    // the in-game Mods tab shows the launcher's locks (pkp.ini [mods]); a pkp.ini that cannot be written does not stop the start
    if let Err(e) = crate::mods_lock::mirror(game_dir, crate::mods_lock::Locks::of(cfg)) {
        eprintln!("mods: the locks were not written to pkp.ini: {e}");
    }

    let look = crate::branding::Branding::of(cfg);
    let app2 = app.clone();
    let dir = game_dir.to_path_buf();
    let tools = crate::paths::exe_dir().join("tools");
    let warm_up = if crate::host::native_game() { crate::host::native_needs_warm_up(game_dir) } else { crate::shaders::needs_warm_up(game_dir) };
    let first = if warm_up {
        "Collecting the shaders the game builds while it runs (once, about a minute)..."
    } else if crate::host::native_game() {
        "Starting the game..."
    } else {
        "Checking the shaders the game builds while it runs..."
    };
    set_status(app, Status { state: GameState::Running, message: first.into(), ..Default::default() });
    std::thread::spawn(move || {
        // every start: the run-time shaders seen so far must all be translated (the warm-up run collects them the first
        // time, e.g. for a setup finished before it existed). A failure here is logged in the status, the game still starts.
        // Linux and macOS translate with their own harness (host::native_prepare); the shader tools are Windows programs
        let prep = if crate::host::native_game() {
            match crate::host::native_prepare(&dir, gentle) {
                Ok(Some(line)) => format!(" {}.", line),
                Ok(None) => String::new(),
                Err(e) => format!(" Shaders were not prepared: {}.", e),
            }
        } else {
            match crate::shaders::prepare(&dir, &tools, &std::sync::atomic::AtomicBool::new(false)) {
                Ok(Some(n)) if n > 0 => format!(" {} shaders were translated before the start.", n),
                Err(e) => format!(" Shaders were not prepared: {}.", e),
                _ => String::new(),
            }
        };
        // Set up and updates write pkp.exe with no icon of ours; the game window takes the exe's icon
        if let Err(e) = crate::branding::ensure_game_icon(&dir, &look) {
            eprintln!("branding: {e}");
        }
        // Calculate after shader preparation, immediately before the process starts, so a long preparation cannot age the offset.
        let mut cmd = match crate::events::override_for(&dir).and_then(|clock| game_command(&exe, &dir, clock, without_mods, coop.as_ref())) {
            Ok(c) => c,
            Err(e) => {
                set_status(&app2, Status { state: GameState::Exited, crashed: true, message: format!("Could not start pkp.exe: {}", e), ..Default::default() });
                return;
            }
        };
        // Cloud Islands (launcher.json cloud_islands): the game is pointed at the server, or at none; in account mode the local gateway
        // runs from here until the game has ended
        let cloud_account = crate::cloud_account::Watch::start(&dir, cloud_islands, &mut cmd);
        let started = SystemTime::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                set_status(&app2, Status { state: GameState::Exited, crashed: true, message: format!("Could not start pkp.exe: {}", e), ..Default::default() });
                return;
            }
        };
        set_status(&app2, Status { state: GameState::Running, message: format!("pkp.exe started (process {})", child.id()), ..Default::default() });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.minimize();
            }
        }
        // the player's save backups: a timer while the game runs (when switched on), one more copy once it has ended
        let backups = crate::savebackup::Watch::start(&dir);
        let presence = crate::presence::Watch::start(discord);
        let online = crate::online::Watch::start(&app2, &dir);
        let result = wait_with_memory_guard(&mut child);
        cloud_account.finish();
        online.finish();
        presence.finish();
        backups.finish();
        let (code, wait_err) = match result {
            Ok(s) => (s.code(), None),
            Err(e) => (None, Some(e.to_string())),
        };
        // shaders the game built while it ran: translated now, so the next run has them (the state stays "running" meanwhile)
        set_status(&app2, Status { state: GameState::Running, message: "Translating the shaders the game built while it ran...".into(), ..Default::default() });
        let shaders = if crate::host::native_game() {
            match crate::host::native_translate_shaders(&dir, &std::sync::atomic::AtomicBool::new(false), gentle, None) {
                Ok(Some(line)) => format!(" {}.", line),
                Ok(None) => String::new(),
                Err(e) => format!(" New shaders were not translated: {}.", e),
            }
        } else {
            match crate::shaders::translate_missing(&dir, &tools) {
                Ok(0) => String::new(),
                Ok(n) => format!(" {} new shaders translated.", n),
                Err(e) => format!(" New shaders were not translated: {}.", e),
            }
        };
        let files = report_files(&dir, started);
        let crash_file = files.iter().any(|f| f.fresh && f.name != "pkp.log");
        let crashed = crash_file || code.map_or(true, |c| c != 0);
        let message = match (&wait_err, code) {
            (Some(e), _) => format!("Lost track of pkp.exe: {}", e),
            (None, Some(0)) => "pkp.exe exited normally.".to_string(),
            (None, Some(c)) => format!("pkp.exe exited with code {} (0x{:08X}).", c, c as u32),
            (None, None) => "pkp.exe was ended by the system.".to_string(),
        };
        set_status(&app2, Status { state: GameState::Exited, code, crashed, files, message: message + &prep + &shaders });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }
    });
    Ok(())
}

/// How Play starts the game: `pkp` in its folder on macOS and Linux, `pkp.exe` on Windows (host.rs). Every path (exefs, romfs, shader
/// cache) comes from pkp.ini next to it. `without_mods`: this start runs without mods. `coop`: the co-op start's environment, or None.
fn game_command(exe: &Path, dir: &Path, clock: Option<crate::events::ClockOverride>, without_mods: bool, coop: Option<&crate::coop_launch::Launch>) -> Result<std::process::Command, String> {
    if !crate::host::native_game() {
        crate::host::sync_runtime_files(&crate::paths::exe_dir(), dir)?;   // pkp_rt.dll and the ASTC decoders: Windows files
    }
    #[cfg(target_os = "macos")]
    crate::host::sync_macos_runtime(dir)?;   // libpkp_rt.dylib and MoltenVK: a launcher update brings newer ones
    // host::command: in the AppImage the game must not inherit LD_LIBRARY_PATH into the AppImage, or the system libcurl the
    // runtime loads for mods' HTTPS requests picks up the AppImage's older libnghttp2 and fails to load (#819)
    let mut cmd = crate::host::command(exe);
    cmd.current_dir(dir).args(["--log", "pkp.log"]);
    // macOS: the folder is not the release's installed layout (no exefs/main next to the binary), so name the settings file;
    // the game reads exefs and romfs from it.
    if crate::host::native_game() && dir.join(crate::settings::PKP_INI).is_file() {
        cmd.args(["--ini", crate::settings::PKP_INI]);
    }
    if crate::host::native_game() {
        // The game writes its log to pkp.log and tees it to stdout. Windows spawns it with CREATE_NO_WINDOW and a GUI-subsystem
        // exe, so nothing shows; on macOS it would inherit the launcher's terminal (or the .app's stderr). Nothing is lost: the
        // log file and the crash/freeze reports are next to the game.
        cmd.stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    }
    if let Some(clock) = clock {
        clock.apply_to(&mut cmd);
    }
    if without_mods {
        leave_mods_out(&mut cmd);
    }
    #[cfg(target_os = "linux")]
    {
        if let Some(size) = mesa_shader_cache_size(std::env::var_os(MESA_SHADER_CACHE_VAR).as_deref()) {
            cmd.env(MESA_SHADER_CACHE_VAR, size);
        }
    }
    crate::coop_launch::apply(&mut cmd, coop);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no stray console if pkp.exe is a console build
    }
    Ok(cmd)
}

/// One start without mods: the runtime's `[mods] off` through its environment override (runtime/docs/CONFIG.md), so pkp.ini and
/// the mods folder are not touched and the next start has its mods again.
fn leave_mods_out(cmd: &mut std::process::Command) {
    cmd.env("NXRT_MODS_OFF", "1");
}

/// Mesa keeps compiled shaders in a disk cache of 1 GB unless MESA_SHADER_CACHE_MAX_SIZE says otherwise. The game's ~99,000
/// translated shaders fill it, and Mesa then evicts and compiles them again (#815). The game gets 4 GB unless the player has
/// set a size; drivers other than Mesa's ignore the variable.
#[cfg(any(target_os = "linux", test))]
const MESA_SHADER_CACHE_VAR: &str = "MESA_SHADER_CACHE_MAX_SIZE";
#[cfg(any(target_os = "linux", test))]
fn mesa_shader_cache_size(players: Option<&std::ffi::OsStr>) -> Option<&'static str> {
    players.is_none().then_some("4G")
}

/// Waits for the game, stopping it if the machine runs out of memory. macOS: the world load needs ~9 GB on a 16 GB host, and
/// the kernel's critical pressure level is the last warning before the machine stops responding (host.rs memory_is_short).
fn wait_with_memory_guard(child: &mut std::process::Child) -> std::io::Result<std::process::ExitStatus> {
    if !crate::host::native_game() {
        return child.wait();
    }
    loop {
        if let Some(s) = child.try_wait()? {
            return Ok(s);
        }
        if crate::host::memory_is_short() {
            eprintln!("the system is out of memory: stopping the game");
            let _ = child.kill();
            return child.wait();
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

pub fn open_with_shell(path: &Path) -> Result<(), String> {
    // explorer (Linux: xdg-open) opens a file in its default app and a folder in a window
    crate::host::open(path.as_os_str())
}

/// Opens an http(s) link in the default browser.
///
/// Windows: not through explorer.exe as open_with_shell does. explorer splits its argument at `=` and `,`, so a link with a
/// query string (the sign-in link: `...?port=..&state=..`) is not taken as a link at all and the Documents folder opens
/// instead. The URL protocol handler takes the link whole, with no shell in between.
pub fn open_url(url: &str) -> Result<(), String> {
    if cfg!(windows) {
        let mut child = crate::host::command("rundll32.exe")
            .arg("url.dll,FileProtocolHandler")
            .arg(url)
            .spawn()
            .map_err(|e| format!("Could not open {url}: {e}"))?;
        std::thread::spawn(move || child.wait());
        return Ok(());
    }
    crate::host::open(std::ffi::OsStr::new(url))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn without_mods_sets_the_runtime_switch() {
        let mut cmd = std::process::Command::new("pkp-test");
        leave_mods_out(&mut cmd);
        let off = cmd.get_envs().find(|(k, _)| *k == std::ffi::OsStr::new("NXRT_MODS_OFF")).and_then(|(_, v)| v);
        assert_eq!(off, Some(std::ffi::OsStr::new("1")));
    }

    #[test]
    fn mesa_cache_size_only_when_the_player_set_none() {
        assert_eq!(mesa_shader_cache_size(None), Some("4G"));
        assert_eq!(mesa_shader_cache_size(Some(std::ffi::OsStr::new("8G"))), None);
    }
}
