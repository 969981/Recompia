// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay server the launcher can host, the LAN and public address lookups and room codes.

use std::fs;
use std::sync::Mutex;
use tauri::AppHandle;

use crate::states::{RelayArg, States};
use crate::config::load_config;
use crate::paths::game_dir_of;
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

/// A new room code for a host, XXXX-XXXX-XXXX-XXXX (80 bits from the OS random source). The launcher writes it to pkp.ini
/// `[net] room_code` so the game hosts with it and the page can show it before the game starts.
#[tauri::command]
pub fn room_code_new() -> Result<String, String> {
    relay::room_code::new_code().map(|c| relay::room_code::format(&c)).map_err(|e| format!("Cannot make a room code: {}", e))
}

/// A visitor's typed code: the display form, or the error text to show.
#[tauri::command]
pub fn room_code_check(code: String) -> Result<String, String> {
    relay::room_code::normalize(&code)
        .map(|c| relay::room_code::format(&c))
        .ok_or_else(|| "A room code has 16 letters and digits: XXXX-XXXX-XXXX-XXXX.".to_string())
}

/// The room code the running game uses, from `<game folder>/room-code.txt` (written by the game when it starts local wireless
/// with `[net] room_code` set, removed when it runs without one). None: no file or no valid code in it.
#[tauri::command]
pub fn room_code_host(app: AppHandle) -> Option<String> {
    let text = fs::read_to_string(game_dir_of(&load_config(&app)).join("room-code.txt")).ok()?;
    relay::room_code::normalize(text.trim()).map(|c| relay::room_code::format(&c))
}
