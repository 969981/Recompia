// SPDX-License-Identifier: GPL-3.0-or-later
//! Cloud Islands (experimental): where the game and the launcher find the Cloud Islands server, and the local account
//! gateway (launcher/cloud-gateway) for the length of one game run.
//!
//! All of it hangs on the launcher's switch (Settings > Game > Online, launcher.json `cloud_islands`, off unless the
//! player switches it on). Off: the launcher shows no sign-in, starts no gateway and points the game at no server, also
//! when pkp.ini names one (the game is told `cloud_server` is empty for that run). On: the game is pointed at
//! [`SERVER`] and the launcher signs in at [`PORTAL`]; pkp.ini's `[net] cloud_server` / `cloud_portal` replace them
//! (a test server). The launcher never writes them to pkp.ini.
//!
//! A server in account mode takes only connections that present this device's certificate; the gateway presents it for
//! the game (docs/cloud-island/README.md). It runs while the switch is on, unless pkp.ini says `[net] cloud_account =
//! off` (a server without accounts). Signed in or not: the gateway reads the device folder (`[net]
//! cloud_account_dir`, else the default one) at every connection, so a sign-in during the game is used at once and,
//! while signed out, the game's connections are closed instead of reaching the server without a device. Failing to
//! start: no gateway, and the game starts pointed at the server without it.
//!
//! The game learns of all this through its environment for this one run (the runtime's `NXRT_NET_*` overrides,
//! runtime/docs/CONFIG.md), so pkp.ini is never written here: the server, the certificate to trust (new at every
//! start) and the two loopback ports. The ports are `[net] cloud_account_tls_port` / `cloud_account_plain_port` when
//! set, else 18443 / 18444; when those are taken (a second game), any free ones.

use recompia_cloud_gateway::{device, Gateway, Options, DEFAULT_PLAIN_PORT, DEFAULT_TLS_PORT};
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// The Cloud Islands server the switch points the game at (an IPv4 address, as the runtime's `[net] cloud_server`).
pub const SERVER: &str = "204.216.107.117";
/// That server's account portal: where the launcher signs this device in, and the account page.
pub const PORTAL: &str = "https://islands.recompia.com:8444";

/// The gateway's lines for this run (one per connection: counts and how it ended, never contents), next to pkp.log.
pub const LOG_FILE: &str = "cloud_gateway.log";

fn get<'a>(pkp: &'a HashMap<String, String>, k: &str) -> Option<&'a str> {
    pkp.get(k).map(|v| v.trim()).filter(|v| !v.is_empty())
}

/// The device folder pkp.ini names, else the default one.
pub fn device_dir(pkp: &HashMap<String, String>) -> PathBuf {
    get(pkp, "net.cloud_account_dir").map(PathBuf::from).unwrap_or_else(device::default_dir)
}

/// The server the game is pointed at: None while the switch is off.
fn server(on: bool, pkp: &HashMap<String, String>) -> Option<String> {
    on.then(|| get(pkp, "net.cloud_server").unwrap_or(SERVER).to_string())
}

/// The account portal's address, unchecked: None while the switch is off (no sign-in anywhere in the launcher).
pub fn portal(on: bool, pkp: &HashMap<String, String>) -> Option<String> {
    on.then(|| get(pkp, "net.cloud_portal").unwrap_or(PORTAL).to_string())
}

/// The gateway the switch and pkp.ini ask for: None when account mode is off.
#[derive(Debug, PartialEq, Eq)]
struct Wanted {
    device_dir: Option<PathBuf>,
    tls_port: u16,
    plain_port: u16,
}

fn wanted(on: bool, pkp: &HashMap<String, String>) -> Option<Wanted> {
    if !on {
        return None;
    }
    if let Some(v) = get(pkp, "net.cloud_account") {
        if !matches!(v.to_ascii_lowercase().as_str(), "on" | "1" | "true" | "yes") {
            return None;
        }
    }
    let port = |k: &str, default: u16| get(pkp, k).and_then(|v| v.parse::<u16>().ok()).filter(|p| *p != 0).unwrap_or(default);
    Some(Wanted {
        device_dir: get(pkp, "net.cloud_account_dir").map(PathBuf::from),
        tls_port: port("net.cloud_account_tls_port", DEFAULT_TLS_PORT),
        plain_port: port("net.cloud_account_plain_port", DEFAULT_PLAIN_PORT),
    })
}

/// The server for this run, in `cmd`'s environment: the switch's when on; off, an empty one when pkp.ini names a
/// server (empty is "off" to the runtime), else nothing at all.
fn point(cmd: &mut std::process::Command, on: bool, pkp: &HashMap<String, String>) {
    match server(on, pkp) {
        Some(s) => {
            cmd.env("NXRT_NET_CLOUD_SERVER", s);
        }
        None if get(pkp, "net.cloud_server").is_some() => {
            cmd.env("NXRT_NET_CLOUD_SERVER", "");
        }
        None => {}
    }
}

/// The gateway for one game run. Does nothing when account mode is off.
pub struct Watch {
    gateway: Option<Gateway>,
}

impl Watch {
    /// Before the game starts: points the game at the server (or at none) and, in account mode, starts the gateway and
    /// tells the game about it, all through `cmd`'s environment. `on`: the launcher's switch (launcher.json `cloud_islands`).
    pub fn start(game_dir: &Path, on: bool, cmd: &mut std::process::Command) -> Watch {
        let pkp = crate::settings::read_pkp(game_dir);
        point(cmd, on, &pkp);
        let Some(want) = wanted(on, &pkp) else { return Watch { gateway: None } };
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
    /// Points another process of the same run (split screen's other windows) as `start` pointed the first: the same server and,
    /// in account mode, the same gateway, which serves every connection of the run.
    pub fn point_also(&self, game_dir: &Path, on: bool, cmd: &mut std::process::Command) {
        point(cmd, on, &crate::settings::read_pkp(game_dir));
        if let Some(g) = &self.gateway {
            apply(cmd, g);
        }
    }

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

    fn env_of(cmd: &std::process::Command, k: &str) -> Option<Option<String>> {
        cmd.get_envs().find(|(n, _)| *n == k).map(|(_, v)| v.map(|v| v.to_string_lossy().into_owned()))
    }

    #[test]
    fn off_points_the_game_at_no_server_and_signs_in_nowhere() {
        let set = ini(&[("net.cloud_server", "127.0.0.1"), ("net.cloud_portal", "https://accounts.example.org"), ("net.cloud_account", "on")]);
        assert_eq!(wanted(false, &set), None);
        assert_eq!(portal(false, &set), None);
        let mut cmd = std::process::Command::new("pkp");
        point(&mut cmd, false, &set);
        assert_eq!(env_of(&cmd, "NXRT_NET_CLOUD_SERVER"), Some(Some(String::new())), "pkp.ini's server is switched off for the run");
        let mut plain = std::process::Command::new("pkp");
        point(&mut plain, false, &ini(&[]));
        assert_eq!(env_of(&plain, "NXRT_NET_CLOUD_SERVER"), None, "nothing to switch off: the environment is left alone");
    }

    #[test]
    fn on_uses_the_built_in_server_and_portal_unless_pkp_ini_names_others() {
        let mut cmd = std::process::Command::new("pkp");
        point(&mut cmd, true, &ini(&[]));
        assert_eq!(env_of(&cmd, "NXRT_NET_CLOUD_SERVER"), Some(Some(SERVER.to_string())));
        assert_eq!(portal(true, &ini(&[])).as_deref(), Some(PORTAL));
        let own = ini(&[("net.cloud_server", " 127.0.0.1 "), ("net.cloud_portal", "https://127.0.0.1:8444")]);
        let mut cmd = std::process::Command::new("pkp");
        point(&mut cmd, true, &own);
        assert_eq!(env_of(&cmd, "NXRT_NET_CLOUD_SERVER"), Some(Some("127.0.0.1".to_string())));
        assert_eq!(portal(true, &own).as_deref(), Some("https://127.0.0.1:8444"));
    }

    #[test]
    fn on_with_default_or_given_ports() {
        assert_eq!(wanted(true, &ini(&[])), Some(Wanted { device_dir: None, tls_port: DEFAULT_TLS_PORT, plain_port: DEFAULT_PLAIN_PORT }));
        let given = [("net.cloud_account", "On"), ("net.cloud_account_tls_port", "20001"), ("net.cloud_account_plain_port", "0"), ("net.cloud_account_dir", "device")];
        assert_eq!(wanted(true, &ini(&given)), Some(Wanted { device_dir: Some(PathBuf::from("device")), tls_port: 20001, plain_port: DEFAULT_PLAIN_PORT }));
    }

    #[test]
    fn account_mode_can_be_switched_off_for_a_server_without_accounts() {
        assert_eq!(wanted(true, &ini(&[("net.cloud_account", "off")])), None);
        assert_eq!(wanted(true, &ini(&[("net.cloud_server", "127.0.0.1"), ("net.cloud_account", "0")])), None);
    }
}
