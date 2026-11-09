// SPDX-License-Identifier: GPL-3.0-or-later
//! What must be idle before a command runs: the game, Setup, an update, a shader repack. One question, `ensure_idle`, with the
//! refusal each command shows its player.

use tauri::AppHandle;

use crate::states::States;
use crate::{play, saveimport};

/// The game is running: pkp.exe started by Play, or found running by name (started some other way).
pub fn game_running(play: &play::PlayState) -> bool {
    play.0.lock().unwrap().state == play::GameState::Running || saveimport::game_running()
}

/// One thing that has to be idle. Game and Setup carry the command's own refusal; an update and a repack read the same everywhere.
pub enum Need {
    Game(&'static str),
    Setup(&'static str),
    /// Play, Set up and the DLSS download write or run what an update is replacing.
    Update,
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
            Need::Storage => app.storage().running.load(std::sync::atomic::Ordering::SeqCst).then_some(STORAGE_BUSY),
        };
        if let Some(msg) = refusal {
            return Err(msg.into());
        }
    }
    Ok(())
}
