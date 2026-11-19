// SPDX-License-Identifier: GPL-3.0-or-later
//! Cloud Islands account mode: the local account gateway (launcher/cloud-gateway) for the length of one game run.
//!
//! A server in account mode takes only connections that present this device's certificate; the gateway presents it for
//! the game (docs/cloud-island/README.md). It runs when pkp.ini has a `cloud_server` and account mode is on: `[net]
//! cloud_account = on`, or, when that line is absent, an account portal is set (`[net] cloud_portal`, which also shows
//! the launcher's Sign in button; cloud_signin.rs). Signed in or not: the gateway reads the device folder (`[net]
//! cloud_account_dir`, else the default one) at every connection, so a sign-in during the game is used at once and,
//! while signed out, the game's connections are closed instead of reaching the server without a device. Off or
//! failing to start: nothing is set and the game starts as it would without this file.
//!
//! The game learns of the gateway through its environment for this one run (the runtime's `NXRT_NET_*` overrides,
//! runtime/docs/CONFIG.md), so pkp.ini is never written here: the certificate to trust (new at every start) and the
//! two loopback ports. The ports are `[net] cloud_account_tls_port` / `cloud_account_plain_port` when set, else
//! 18443 / 18444; when those are taken (a second game), any free ones.

use recompia_cloud_gateway::{device, Gateway, Options, DEFAULT_PLAIN_PORT, DEFAULT_TLS_PORT};
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// The gateway's lines for this run (one per connection: counts and how it ended, never contents), next to pkp.log.
pub const LOG_FILE: &str = "cloud_gateway.log";

fn get<'a>(pkp: &'a HashMap<String, String>, k: &str) -> Option<&'a str> {
    pkp.get(k).map(|v| v.trim()).filter(|v| !v.is_empty())
}

/// The device folder pkp.ini names, else the default one.
pub fn device_dir(pkp: &HashMap<String, String>) -> PathBuf {
    get(pkp, "net.cloud_account_dir").map(PathBuf::from).unwrap_or_else(device::default_dir)
}

/// The account portal's address from pkp.ini, unchecked; None hides the sign-in.
pub fn portal(pkp: &HashMap<String, String>) -> Option<String> {
    get(pkp, "net.cloud_portal").map(str::to_string)
}

/// What pkp.ini asks for: None when account mode is off.
#[derive(Debug, PartialEq, Eq)]
struct Wanted {
    device_dir: Option<PathBuf>,
    tls_port: u16,
    plain_port: u16,
}

fn wanted(pkp: &HashMap<String, String>) -> Option<Wanted> {
    get(pkp, "net.cloud_server")?;
    let on = match get(pkp, "net.cloud_account") {
        Some(v) => matches!(v.to_ascii_lowercase().as_str(), "on" | "1" | "true" | "yes"),
        None => portal(pkp).is_some(),
    };
    if !on {
        return None;
    }
    let port = |k: &str, default: u16| get(pkp, k).and_then(|v| v.parse::<u16>().ok()).filter(|p| *p != 0).unwrap_or(default);
    Some(Wanted {
        device_dir: get(pkp, "net.cloud_account_dir").map(PathBuf::from),
        tls_port: port("net.cloud_account_tls_port", DEFAULT_TLS_PORT),
        plain_port: port("net.cloud_account_plain_port", DEFAULT_PLAIN_PORT),
    })
}

/// The gateway for one game run. Does nothing when account mode is off.
pub struct Watch {
    gateway: Option<Gateway>,
}

impl Watch {
    /// Before the game starts: starts the gateway and tells the game about it through `cmd`'s environment.
    pub fn start(game_dir: &Path, cmd: &mut std::process::Command) -> Watch {
        let Some(want) = wanted(&crate::settings::read_pkp(game_dir)) else { return Watch { gateway: None } };
        let device_dir = want.device_dir.unwrap_or_else(device::default_dir);
        let file = std::fs::File::create(game_dir.join(LOG_FILE)).ok().map(Mutex::new);
        let log: recompia_cloud_gateway::Log = Arc::new(move |line: &str| {
            if let Some(f) = &file {
                let _ = writeln!(f.lock().unwrap_or_else(|e| e.into_inner()), "{} {line}", chrono::Local::now().format("%H:%M:%S"));
            }
        });
        let options = |tls_port, plain_port| Options { device_dir: device_dir.clone(), tls_port, plain_port, ca_file: None, log: log.clone() };
        // the wanted ports first; taken (another game's gateway): any free ones
        let started = recompia_cloud_gateway::start(options(want.tls_port, want.plain_port)).or_else(|first| {
            eprintln!("cloud account: {first}; trying free ports");
            recompia_cloud_gateway::start(options(0, 0))
        });
        match started {
            Ok(g) => {
                apply(cmd, &g);
                Watch { gateway: Some(g) }
            }
            Err(e) => {
                eprintln!("cloud account: the gateway did not start: {e}");
                Watch { gateway: None }
            }
        }
    }

    /// The game has ended: closes the listeners and every connection.
    pub fn finish(self) {
        if let Some(g) = self.gateway {
            g.stop();
        }
    }
}

fn apply(cmd: &mut std::process::Command, g: &Gateway) {
    cmd.env("NXRT_NET_CLOUD_CA", &g.ca_file)
        .env("NXRT_NET_CLOUD_ACCOUNT_TLS_PORT", g.tls_port.to_string())
        .env("NXRT_NET_CLOUD_ACCOUNT_PLAIN_PORT", g.plain_port.to_string());
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ini(pairs: &[(&str, &str)]) -> HashMap<String, String> {
        pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    }

    #[test]
    fn off_without_the_switch_or_the_server() {
        assert_eq!(wanted(&ini(&[("net.cloud_server", "127.0.0.1")])), None);
        assert_eq!(wanted(&ini(&[("net.cloud_server", "127.0.0.1"), ("net.cloud_account", "off")])), None);
        assert_eq!(wanted(&ini(&[("net.cloud_account", "on")])), None);
        assert_eq!(wanted(&ini(&[("net.cloud_portal", "https://accounts.example.org")])), None);
    }

    #[test]
    fn on_with_default_or_given_ports() {
        let base = [("net.cloud_server", "127.0.0.1"), ("net.cloud_account", "On")];
        assert_eq!(wanted(&ini(&base)), Some(Wanted { device_dir: None, tls_port: DEFAULT_TLS_PORT, plain_port: DEFAULT_PLAIN_PORT }));
        let mut given = base.to_vec();
        given.extend([("net.cloud_account_tls_port", "20001"), ("net.cloud_account_plain_port", "0"), ("net.cloud_account_dir", "device")]);
        assert_eq!(wanted(&ini(&given)), Some(Wanted { device_dir: Some(PathBuf::from("device")), tls_port: 20001, plain_port: DEFAULT_PLAIN_PORT }));
    }

    #[test]
    fn a_portal_switches_it_on_unless_switched_off() {
        let base = [("net.cloud_server", "127.0.0.1"), ("net.cloud_portal", "https://accounts.example.org")];
        assert_eq!(wanted(&ini(&base)), Some(Wanted { device_dir: None, tls_port: DEFAULT_TLS_PORT, plain_port: DEFAULT_PLAIN_PORT }));
        let mut off = base.to_vec();
        off.push(("net.cloud_account", "off"));
        assert_eq!(wanted(&ini(&off)), None);
    }
}
