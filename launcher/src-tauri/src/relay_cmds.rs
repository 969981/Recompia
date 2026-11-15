// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay server the launcher can host, and the LAN and public address lookups.

use std::sync::Mutex;
use tauri::AppHandle;

use crate::states::{RelayArg, States};
use crate::{lan, public_ip, relay};

pub fn stop_relay(app: &AppHandle) {
    // take it out first: stopping joins threads, the lock is not held meanwhile
    let srv = app.relay().0.lock().unwrap().take();
    if let Some(mut srv) = srv {
        srv.stop();
    }
}

/// The relay server the launcher runs on a thread (Host button, phase 4). None: not running.
#[derive(Default)]
pub struct RelayState(Mutex<Option<relay::RelayServer>>);

/// Starts the relay (pokopia-native's coop-relay protocol) on 0.0.0.0:port (0 or none: 47610). A running one is stopped first.
#[tauri::command]
pub fn relay_start(state: RelayArg, port: Option<u16>) -> Result<relay::Status, String> {
    let mut slot = state.0.lock().unwrap();
    if let Some(mut old) = slot.take() {
        old.stop();
    }
    let port = port.filter(|p| *p != 0).unwrap_or(relay::DEFAULT_PORT);
    let srv = relay::RelayServer::start(("0.0.0.0", port), relay::Config::default(), 0).map_err(|e| format!("Cannot open port {}: {}", port, e))?;
    let status = srv.status();
    *slot = Some(srv);
    Ok(status)
}

#[tauri::command]
pub fn relay_stop(app: AppHandle) {
    stop_relay(&app);
}

#[tauri::command]
pub fn relay_status(state: RelayArg) -> Option<relay::Status> {
    state.0.lock().unwrap().as_ref().map(|s| s.status())
}

/// LAN address to give visitors, other addresses, and the Windows Public-network warning (read only).
#[tauri::command]
pub fn lan_report() -> lan::LanReport {
    lan::report()
}

/// This PC's public IPv4 address for an internet host's visitors, or None when it cannot be found (offline, a service down).
#[tauri::command]
pub async fn public_ip() -> Option<String> {
    tauri::async_runtime::spawn_blocking(public_ip::lookup).await.ok().flatten()
}
