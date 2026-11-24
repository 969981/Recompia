// SPDX-License-Identifier: GPL-3.0-or-later
//! Signing this device in to a Cloud Islands account (launcher/cloud-gateway signin.rs), for the sign-in screen the
//! launcher starts on and Home's account chip opens (launcher/src/pages/welcome.js, cloud-signin.js).
//!
//! Only while Cloud Islands are switched on (Settings > Game > Online, launcher.json `cloud_islands`): off, the state is
//! "off", the chip is hidden and nothing here runs. The portal is cloud_account.rs `PORTAL`, or pkp.ini's `[net]
//! cloud_portal` (a test server).
//! Signed in, the device's four files are in the device folder (`[net] cloud_account_dir`, else the default one) and
//! the account gateway (cloud_account.rs) presents them for the game; a running gateway notices a sign-in or sign-out
//! by itself.
//!
//! Commands (also what an in-game trigger would call through the runtime API: begin, then read the state about once a
//! second until it is no longer "waiting"):
//! - `cloud_signin_state`: `{ state: off | signed_out | waiting | signed_in | expired, ... }` (see `state`)
//! - `cloud_signin_begin { method: "browser" | "phone" | null }`: starts a sign-in (null: this platform's default)
//! - `cloud_signin_cancel`: stops waiting ("Not now")
//! - `cloud_sign_out`: removes the sign-in from this device
//! - `cloud_account_page`: opens the account page (where devices are removed, which revokes them)

use crate::config::load_config;
use recompia_cloud_gateway::device;
use recompia_cloud_gateway::signin::{self, Flow, Method, Phase};
use serde_json::{json, Value};
use std::sync::Mutex;
use tauri::AppHandle;

/// The sign-in in progress (or the last one, until it is read as ended and cleared by the next begin or cancel).
static FLOW: Mutex<Option<Flow>> = Mutex::new(None);

fn flow() -> std::sync::MutexGuard<'static, Option<Flow>> {
    FLOW.lock().unwrap_or_else(|e| e.into_inner())
}

struct Setup {
    portal: Option<Result<String, String>>,
    dir: std::path::PathBuf,
}

fn setup(app: &AppHandle) -> Setup {
    let cfg = load_config(app);
    let pkp = crate::settings::read_pkp(&crate::paths::game_dir_of(&cfg));
    Setup {
        portal: crate::cloud_account::portal(cfg.cloud_islands, &pkp).map(|p| signin::check_portal(&p)),
        dir: crate::cloud_account::device_dir(&pkp),
    }
}

/// What the sign-in screen shows. Always: `state`, `default_method`, `device_name`. Signed in or expired: `account_name`
/// (falling back to the account id), `not_after` (Unix seconds); signed in: `needs_passkey` (cloud_passkey.rs). Waiting: `method` and the phase (`phase.phase` is
/// starting | browser | phone | finishing; phone has `url`, `user_code`, `qr_svg` (an SVG made here), `fingerprint`,
/// `expires_in`). After a failure: `error`. An unusable portal address: `portal_error`.
fn state(s: &Setup) -> Value {
    let base = |state: &str| json!({ "state": state, "default_method": signin::default_method().name(), "device_name": signin::device_name() });
    let portal = match &s.portal {
        None => return base("off"),
        Some(Err(e)) => {
            let mut v = base("off");
            v["portal_error"] = json!(e);
            return v;
        }
        Some(Ok(p)) => p,
    };
    let mut last_error = None;
    if let Some(f) = flow().as_ref() {
        let phase = f.phase();
        match &phase {
            Phase::Failed(e) => last_error = Some(e.clone()),
            Phase::Done(_) | Phase::Cancelled => {}
            _ => {
                let mut v = base("waiting");
                v["method"] = json!(f.method.name());
                v["phase"] = phase.to_json();
                return v;
            }
        }
    }
    let mut v = match device::summary(&s.dir) {
        Ok(sum) => {
            let mut v = base(if signin::expired(sum.not_after) { "expired" } else { "signed_in" });
            v["account_name"] = json!(if sum.account_name.is_empty() { &sum.account } else { &sum.account_name });
            v["not_after"] = json!(sum.not_after);
            let other = !sum.portal.is_empty() && sum.portal.trim_end_matches('/') != portal;
            v["other_portal"] = json!(other);
            // its old passkeys died with the portal's old address: cloud_passkey.rs
            v["needs_passkey"] = json!(!signin::expired(sum.not_after) && crate::cloud_passkey::needed(&s.dir, portal, &sum.device, other));
            v
        }
        Err(_) => base("signed_out"),
    };
    if let Some(e) = last_error {
        v["error"] = json!(e);
    }
    v
}

/// The portal and the device folder, for a command that needs both: an error while Cloud Islands are off.
pub fn portal_and_dir(app: &AppHandle) -> Result<(String, std::path::PathBuf), String> {
    let s = setup(app);
    match s.portal {
        Some(Ok(p)) => Ok((p, s.dir)),
        Some(Err(e)) => Err(e),
        None => Err("Cloud Islands are switched off (Settings > Game > Online).".into()),
    }
}

#[tauri::command]
pub fn cloud_signin_state(app: AppHandle) -> Value {
    state(&setup(&app))
}

#[tauri::command]
pub fn cloud_signin_begin(app: AppHandle, method: Option<String>) -> Result<Value, String> {
    let s = setup(&app);
    let portal = match &s.portal {
        Some(Ok(p)) => p.clone(),
        Some(Err(e)) => return Err(e.clone()),
        None => return Err("Cloud Islands are switched off (Settings > Game > Online).".into()),
    };
    let method = match method.as_deref() {
        None | Some("") => signin::default_method(),
        Some(m) => Method::parse(m).ok_or_else(|| format!("unknown sign-in method {m}"))?,
    };
    let mut slot = flow();
    if let Some(old) = slot.take() {
        old.cancel();
    }
    *slot = Some(Flow::start(&portal, s.dir.clone(), method, Box::new(|url: &str| crate::play::open_url(url)))?);
    drop(slot);
    Ok(state(&s))
}

#[tauri::command]
pub fn cloud_signin_cancel(app: AppHandle) -> Value {
    if let Some(f) = flow().take() {
        f.cancel();
    }
    state(&setup(&app))
}

#[tauri::command]
pub fn cloud_sign_out(app: AppHandle) -> Result<Value, String> {
    if let Some(f) = flow().take() {
        f.cancel();
    }
    let s = setup(&app);
    device::remove(&s.dir)?;
    Ok(state(&s))
}

#[tauri::command]
pub fn cloud_account_page(app: AppHandle) -> Result<(), String> {
    match setup(&app).portal {
        Some(Ok(p)) => crate::play::open_url(&format!("{p}/")),
        Some(Err(e)) => Err(e),
        None => Err("Cloud Islands are switched off (Settings > Game > Online).".into()),
    }
}
