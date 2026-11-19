// SPDX-License-Identifier: GPL-3.0-or-later
//! Controllers read natively (gilrs), for a webview without the Gamepad API: WebKitGTK on Linux and the Steam Deck often has none, so
//! the page's controller navigation (src/gamepad-nav.js) would never see a pad there. On Linux only (WebView2 and WKWebView have the
//! API), a thread reads the first connected pad about 60 times a second while the launcher's window has the focus, and emits
//! `pad-state` when anything changed; `pad_poll` answers the current state. While a game has the focus it only checks the focus, four
//! times a second. The page uses these only when navigator.getGamepads() gives no pad.
//!
//! The layout is the W3C "standard" gamepad's, the one the page reads from the browser: buttons A, B, X, Y, LB, RB, LT, RT, Back, Start,
//! LS, RS, Up, Down, Left, Right, Guide; axes left x, left y, right x, right y, with y positive downwards.

use serde::Serialize;
use std::sync::{LazyLock, Mutex};
use tauri::AppHandle;

pub const BUTTONS: usize = 17;

#[derive(Serialize, Clone, Debug, Default, PartialEq)]
pub struct PadState {
    pub connected: bool,
    pub buttons: [bool; BUTTONS],
    pub axes: [f32; 4],
}

static CURRENT: LazyLock<Mutex<PadState>> = LazyLock::new(|| Mutex::new(PadState::default()));

#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const DEAD: f32 = 0.05;

#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
/// An axis as sent: small values are 0 and the rest rounded to 1/100, so stick noise does not send an event every frame.
pub fn quantize(v: f32) -> f32 {
    if !v.is_finite() || v.abs() < DEAD {
        0.0
    } else {
        (v.clamp(-1.0, 1.0) * 100.0).round() / 100.0
    }
}

#[tauri::command]
pub fn pad_poll() -> PadState {
    CURRENT.lock().unwrap().clone()
}

/// Elsewhere the webview reads the pads itself.
#[cfg(not(target_os = "linux"))]
pub fn start(_app: AppHandle) {}

/// Starts the reading thread. A system where gilrs cannot start (no input subsystem) just has no native pad.
#[cfg(target_os = "linux")]
pub fn start(app: AppHandle) {
    use std::time::Duration;
    use tauri::{Emitter, Manager};
    std::thread::Builder::new()
        .name("pad".into())
        .spawn(move || {
            use gilrs::{Axis, Button, Gilrs};
            let mut gilrs = match Gilrs::new() {
                Ok(g) => g,
                Err(e) => {
                    eprintln!("pad: no native controller input: {e}");
                    return;
                }
            };
            const ORDER: [Button; BUTTONS] = [
                Button::South,
                Button::East,
                Button::West,
                Button::North,
                Button::LeftTrigger,
                Button::RightTrigger,
                Button::LeftTrigger2,
                Button::RightTrigger2,
                Button::Select,
                Button::Start,
                Button::LeftThumb,
                Button::RightThumb,
                Button::DPadUp,
                Button::DPadDown,
                Button::DPadLeft,
                Button::DPadRight,
                Button::Mode,
            ];
            let focused = || app.get_webview_window("main").and_then(|w| w.is_focused().ok()).unwrap_or(true);
            let mut tick = 0u32;
            let mut active = true;
            loop {
                if tick % 30 == 0 {
                    active = focused();
                }
                tick = tick.wrapping_add(1);
                while gilrs.next_event().is_some() {}
                let mut s = PadState::default();
                if !active {
                    // a button held as the focus left would stay pressed in the page: it is sent released once
                    let mut cur = CURRENT.lock().unwrap();
                    if *cur != s {
                        *cur = s.clone();
                        let _ = app.emit("pad-state", s);
                    }
                    drop(cur);
                    tick = 0;
                    std::thread::sleep(Duration::from_millis(250));
                    continue;
                }
                if let Some((_, pad)) = gilrs.gamepads().find(|(_, g)| g.is_connected()) {
                    s.connected = true;
                    for (i, b) in ORDER.iter().enumerate() {
                        s.buttons[i] = pad.is_pressed(*b) || pad.button_data(*b).is_some_and(|d| d.value() > 0.5);
                    }
                    // gilrs counts y upwards; the browser downwards
                    s.axes = [
                        quantize(pad.value(Axis::LeftStickX)),
                        quantize(-pad.value(Axis::LeftStickY)),
                        quantize(pad.value(Axis::RightStickX)),
                        quantize(-pad.value(Axis::RightStickY)),
                    ];
                }
                let changed = {
                    let mut cur = CURRENT.lock().unwrap();
                    let changed = *cur != s;
                    if changed {
                        *cur = s.clone();
                    }
                    changed
                };
                if changed {
                    let _ = app.emit("pad-state", s);
                }
                std::thread::sleep(Duration::from_millis(16));
            }
        })
        .ok();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn axes_drop_noise_and_round() {
        assert_eq!(quantize(0.03), 0.0);
        assert_eq!(quantize(-0.049), 0.0);
        assert_eq!(quantize(0.556), 0.56);
        assert_eq!(quantize(-2.0), -1.0);
        assert_eq!(quantize(f32::NAN), 0.0);
        let sent = serde_json::to_value(PadState::default()).unwrap();
        assert_eq!(sent["buttons"].as_array().unwrap().len(), BUTTONS);
        assert_eq!(sent["axes"].as_array().unwrap().len(), 4);
    }
}
