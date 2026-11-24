// SPDX-License-Identifier: GPL-3.0-or-later
//! The online account's passkey, when the account portal moved to another address.
//!
//! A passkey belongs to the portal's host name, so after a move the passkeys made before are dead: the account can no
//! longer sign in, and this device's sign-in is the only way into it that is left. The launcher then asks for a new
//! passkey (Home's chip and the sign-in screen, cloud-signin-model.js) and hides Sign out until there is one. The work
//! is the gateway crate's (`recompia_cloud_gateway::recover`): this file keeps what the portal last said, so the
//! sign-in state never waits on the network, and opens the portal's one-time page.
//!
//! Before the portal has answered, a sign-in that names another portal than today's (`other`: launcher.json was
//! written at the old address) counts as needing one. Once the portal says the account is fine, the sign-in is noted
//! as belonging to today's portal, so that guess is not made again.

use recompia_cloud_gateway::{device, recover};
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::AppHandle;

/// While a new passkey is needed, how often the portal is asked again (the player may just have made it).
const AGAIN: Duration = Duration::from_secs(8);
/// After the portal could not be reached.
const RETRY: Duration = Duration::from_secs(60);

struct Known {
    /// The device and the portal the answer is about.
    key: String,
    needed: Option<bool>,
    asked: Instant,
    asking: bool,
}

static KNOWN: Mutex<Option<Known>> = Mutex::new(None);

fn known() -> std::sync::MutexGuard<'static, Option<Known>> {
    KNOWN.lock().unwrap_or_else(|e| e.into_inner())
}

fn learned(key: &str, dir: &Path, portal: &str, answer: Result<bool, String>) {
    let mut slot = known();
    let Some(k) = slot.as_mut().filter(|k| k.key == key) else { return };
    k.asking = false;
    k.asked = Instant::now();
    match answer {
        Ok(needed) => {
            k.needed = Some(needed);
            if !needed && device::summary(dir).is_ok_and(|s| s.portal.trim_end_matches('/') != portal) {
                if let Err(e) = device::set_portal(dir, portal) {
                    eprintln!("cloud account: {e}");
                }
            }
        }
        Err(e) => eprintln!("cloud account: could not ask the portal about the passkey: {e}"),
    }
}

/// Whether the signed-in account needs a new passkey, as far as is known now; asks the portal in the background when
/// the answer is missing or may have changed. `device`: the device's id; `other`: the sign-in names another portal.
pub fn needed(dir: &Path, portal: &str, device: &str, other: bool) -> bool {
    let key = format!("{device}\n{portal}");
    let mut slot = known();
    if slot.as_ref().is_none_or(|k| k.key != key) {
        *slot = Some(Known { key: key.clone(), needed: None, asked: Instant::now(), asking: false });
        ask(slot.as_mut(), &key, dir, portal);
        return other;
    }
    let k = slot.as_mut().expect("set above");
    let due = match k.needed {
        Some(false) => false,
        Some(true) => k.asked.elapsed() >= AGAIN,
        None => k.asked.elapsed() >= RETRY,
    };
    let needed = k.needed.unwrap_or(other);
    if due {
        ask(slot.as_mut(), &key, dir, portal);
    }
    needed
}

fn ask(k: Option<&mut Known>, key: &str, dir: &Path, portal: &str) {
    let Some(k) = k.filter(|k| !k.asking) else { return };
    k.asking = true;
    let (key, dir, portal): (String, PathBuf, String) = (key.into(), dir.into(), portal.into());
    std::thread::spawn(move || learned(&key, &dir, &portal, recover::needed(&portal, &dir)));
}

/// Opens the portal's one-time page that makes the account a new passkey, in this PC's browser.
#[tauri::command]
pub async fn cloud_passkey_begin(app: AppHandle) -> Result<Value, String> {
    let (portal, dir) = crate::cloud_signin::portal_and_dir(&app)?;
    let link = tauri::async_runtime::spawn_blocking({
        let (portal, dir) = (portal.clone(), dir.clone());
        move || recover::link(&portal, &dir)
    })
    .await
    .map_err(|e| e.to_string())??;
    match link {
        Some(url) => crate::play::open_url(&url)?,
        // it has one already (made on another device, perhaps): say so at the next state
        None => {
            if let Ok(s) = device::summary(&dir) {
                learned(&format!("{}\n{portal}", s.device), &dir, &portal, Ok(false));
            }
        }
    }
    Ok(crate::cloud_signin::cloud_signin_state(app))
}
