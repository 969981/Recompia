// SPDX-License-Identifier: GPL-3.0-or-later
//! Refusals shared by the commands that must not run beside an update.

use std::sync::Arc;
use tauri::{AppHandle, Manager};

use crate::update;

/// Play, Set up and the DLSS download write or run what an update is replacing.
pub fn refuse_during_update(app: &AppHandle) -> Result<(), String> {
    if app.state::<Arc<update::UpdateState>>().busy() {
        return Err("An update is being installed. Wait for it to finish.".into());
    }
    Ok(())
}
