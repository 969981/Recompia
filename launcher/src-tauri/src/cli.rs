// SPDX-License-Identifier: GPL-3.0-or-later
//! The command line modes: the relay alone (--host) and the game without a window (--play).

use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use crate::paths::exe_dir;
use crate::{lan, play, relay};

/// `--host [--listen <IPv4>[:<port>]] [--port N] [--no-udp] [limits] [--stats S] [--quiet]`: the relay alone, no window, log lines on stdout,
/// until Ctrl+C. Listens on 0.0.0.0:47610 by default. The limits are pokopia-native's coop-relay options (--rooms --peers --links --per-ip
/// --new-per-ip --nofind-per-min --room-hours --room-mib --link-kib --wait-s --idle-s). `--no-udp`: no relay UDP (game datagrams stay on the
/// TCP links, as with pokopia-native's relay).
pub fn run_headless(args: &[String]) -> i32 {
    #[cfg(windows)]
    win_console::attach_and_hook_ctrl_c();
    const USAGE: &str = "Usage: PokopiaLauncher --host [--listen <IPv4>[:<port>]] [--port N] [--no-udp] [--rooms N] [--peers N] [--links N] [--per-ip N] [--new-per-ip N] [--nofind-per-min N] [--room-hours H] [--room-mib M] [--link-kib K] [--wait-s S] [--idle-s S] [--stats S] [--quiet]";
    let mut ip = std::net::Ipv4Addr::UNSPECIFIED;
    let mut port = relay::DEFAULT_PORT;
    let mut cfg = relay::Config::default();
    let mut stats_s: u64 = 60;
    let mut quiet = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--host" => continue,
            "--no-udp" => {
                cfg.udp = false;
                continue;
            }
            "--quiet" => {
                quiet = true;
                continue;
            }
            _ => {}
        }
        let Some(v) = it.next() else {
            eprintln!("{} needs a value. {}", a, USAGE);
            return 2;
        };
        if a == "--listen" {
            let (h, p) = match v.split_once(':') {
                Some((h, p)) => (h, Some(p)),
                None => (v.as_str(), None),
            };
            match (h.parse::<std::net::Ipv4Addr>(), p.map(|p| p.parse::<u16>())) {
                (Ok(h), None) => ip = h,
                (Ok(h), Some(Ok(p))) => {
                    ip = h;
                    port = p;
                }
                _ => {
                    eprintln!("--listen {}: <IPv4>[:<port>] wanted", v);
                    return 2;
                }
            }
            continue;
        }
        let Ok(x) = v.parse::<u64>() else {
            eprintln!("{} {}: a number >= 0 wanted", a, v);
            return 2;
        };
        match a.as_str() {
            "--port" if x <= 65535 => port = x as u16,
            "--rooms" => cfg.max_rooms = x as usize,
            "--peers" => cfg.max_peers = x as usize,
            "--links" => cfg.max_conns = x as usize,
            "--per-ip" => cfg.max_per_ip = x.min(i32::MAX as u64) as i32,
            "--new-per-ip" => cfg.new_per_ip = x as i64,
            "--nofind-per-min" => cfg.nofind_per_min = x as i64,
            "--room-hours" => cfg.room_ms = x as i64 * 3600 * 1000,
            "--room-mib" => cfg.room_bytes = x << 20,
            "--link-kib" => cfg.link_bps = (x << 10) as i64,
            "--wait-s" => cfg.wait_ms = x as i64 * 1000,
            "--idle-s" => cfg.idle_ms = x as i64 * 1000,
            "--stats" => stats_s = x,
            _ => {
                eprintln!("unknown argument {}. {}", a, USAGE);
                return 2;
            }
        }
    }
    let log: Arc<dyn Fn(&str) + Send + Sync> = Arc::new(move |line: &str| {
        if !quiet {
            println!("{}", line)
        }
    });
    cfg.log = Some(log);
    let mut srv = match relay::RelayServer::start((ip, port), cfg, stats_s) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cannot listen on {}:{}: {}", ip, port, e);
            return 1;
        }
    };
    println!("Pokopia relay (pokopia-native coop-relay protocol) running on {}. Ctrl+C stops it.", srv.local_addr());
    println!("Anyone who can reach this address can open rooms here; rooms are authenticated end to end by the players, but their traffic is not encrypted.");
    for line in lan::log_lines(&lan::report(), srv.local_addr().port()) {
        println!("{}", line);
    }
    while !win_console::CTRL_C.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
    srv.stop();
    0
}

/// A GUI-subsystem exe has no console: attach to the terminal that started it, and turn Ctrl+C into a clean stop.
mod win_console {
    use std::sync::atomic::AtomicBool;
    pub static CTRL_C: AtomicBool = AtomicBool::new(false);

    #[cfg(windows)]
    pub fn attach_and_hook_ctrl_c() {
        use std::sync::atomic::Ordering;
        const ATTACH_PARENT_PROCESS: u32 = u32::MAX;
        extern "system" {
            fn AttachConsole(pid: u32) -> i32;
            fn SetConsoleCtrlHandler(handler: Option<unsafe extern "system" fn(u32) -> i32>, add: i32) -> i32;
        }
        unsafe extern "system" fn on_ctrl(_kind: u32) -> i32 {
            CTRL_C.store(true, Ordering::SeqCst);
            1 // handled: main() stops the server and returns
        }
        // fails harmlessly when there is no parent console or the exe already has one (debug builds)
        unsafe {
            AttachConsole(ATTACH_PARENT_PROCESS);
            SetConsoleCtrlHandler(Some(on_ctrl), 1);
        }
    }
}

/// `--play [--game <folder>]`: starts the game as Play does, without the window (a Steam or Sunshine shortcut). The game
/// folder is the launcher's own unless given: the one set in Settings lives in the window's config, not read here.
pub fn run_play(args: &[String]) -> i32 {
    #[cfg(windows)]
    win_console::attach_and_hook_ctrl_c();
    let game = args.iter().position(|a| a == "--game").and_then(|i| args.get(i + 1)).map(PathBuf::from).unwrap_or_else(exe_dir);
    play::run_cli(&game, &exe_dir().join("tools"))
}
