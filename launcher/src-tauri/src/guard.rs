// SPDX-License-Identifier: GPL-3.0-or-later
//! What must be idle before a command runs: the game, Setup, an update, a shader repack. One question, `ensure_idle`, with the
//! refusal each command shows its player.

use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::AppHandle;

use crate::states::States;
use crate::{play, saveimport};

/// How long a look at the process list is trusted. The list is only asked about a pkp.exe nobody started through Play (Play's own
/// state is always read live), and it costs a `tasklist` process per call. Two seconds is safe: every command that asks is a click,
/// a pkp.exe started by hand a moment after a click cannot be told from one started a moment later anyway, and a game that just
/// closed makes the next refusal last at most two seconds longer.
const PROCESS_LOOK_TTL: Duration = Duration::from_secs(2);

fn pkp_process_running() -> bool {
    static LAST: Mutex<Option<(Instant, bool)>> = Mutex::new(None);
    let mut last = LAST.lock().unwrap();
    if let Some((at, found)) = *last {
        if at.elapsed() < PROCESS_LOOK_TTL {
            return found;
        }
    }
    let found = saveimport::game_running();
    *last = Some((Instant::now(), found));
    found
}

/// The game is running: pkp.exe started by Play, or found running by name (started some other way).
pub fn game_running(play: &play::PlayState) -> bool {
    play.0.lock().unwrap().state == play::GameState::Running || pkp_process_running()
}

/// One thing that has to be idle. Game and Setup carry the command's own refusal; an update and a repack read the same everywhere.
pub enum Need {
    Game(&'static str),
    Setup(&'static str),
    /// Set up, the DLSS download and the other jobs that write or run what an update is replacing: refused for the whole update.
    Update,
    /// Play: refused only once the update has begun to move files (update.rs `applying`), so the game stays playable while the
    /// update downloads and prepares. The caller holds `UpdateState::hold_swap` from this check until its game has started.
    Swap,
    Storage,
}

const UPDATE_BUSY: &str = "An update is being installed. Wait for it to finish.";
const STORAGE_BUSY: &str = "The shader files are being repacked. Wait for it to finish, or stop it in Settings.";

/// Ok when everything in `needs` is idle; else the refusal of the first that is not, in the order given.
pub fn ensure_idle(app: &AppHandle, needs: &[Need]) -> Result<(), String> {
    for need in needs {
        let refusal = match need {
            Need::Game(msg) => game_running(&app.play()).then_some(*msg),
            Need::Setup(msg) => app.setup().running().then_some(*msg),
            Need::Update => app.updates().busy().then_some(UPDATE_BUSY),
            Need::Swap => app.updates().applying().then_some(UPDATE_BUSY),
            Need::Storage => app.storage().running.load(std::sync::atomic::Ordering::SeqCst).then_some(STORAGE_BUSY),
        };
        if let Some(msg) = refusal {
            return Err(msg.into());
        }
    }
    Ok(())
}
