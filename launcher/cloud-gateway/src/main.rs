// SPDX-License-Identifier: GPL-3.0-or-later
//! The online account as a program of its own (handhelds, servers without the launcher, tests): signing this device in
//! or out, and the account gateway, which runs until it is stopped. The game's pkp.ini needs the three `[net]` lines
//! the gateway prints at start.

use recompia_cloud_gateway::signin::{self, Method, Phase};
use recompia_cloud_gateway::{device, recover, start, Options, DEFAULT_PLAIN_PORT, DEFAULT_TLS_PORT};
use std::path::PathBuf;
use std::sync::Arc;

const USAGE: &str = "recompia-cloud-gateway [run] [--home <device folder>] [--server <address>] [--tls-port <n>] [--plain-port <n>] [--ca-out <file.der>]
recompia-cloud-gateway signin --portal <https://...> [--home <device folder>] [--method browser|phone] [--no-open]
recompia-cloud-gateway status [--home <device folder>]
recompia-cloud-gateway signout [--home <device folder>]
recompia-cloud-gateway recover --portal <https://...> [--home <device folder>] [--no-open]

  run           the gateway (the default). It starts signed in or not, and picks up a sign-in or sign-out in the
                device folder by itself, without a restart
  signin        signs this device in to the account portal: browser = approve in this PC's browser (default on
                Windows), phone = scan a QR code and type the short code on a phone (default elsewhere)
  status        who this device is signed in as
  signout       removes this device's sign-in from the device folder (remove the device on the portal to revoke it)
  recover       a new passkey for this device's account when its old ones stopped working (the portal moved to
                another address): opens the portal's one-time page for it in this PC's browser

  --home        the device folder (launcher.json, device.crt, device.key, ingress-ca.pem);
                default: where the server's launcher.py link command puts it
  --server      run: the server's address, dialled instead of the host the sign-in stored (the port and the
                certificate name stay the sign-in's)
  --tls-port    127.0.0.1 port for the game's TLS connections (default 18443, 0 = any free port)
  --plain-port  127.0.0.1 port for the island file (default 18444, 0 = any free port)
  --ca-out      where to write the certificate the game must trust; default: recompia-gateway-ca.der in --home
  --no-open     signin, recover: print the address instead of opening a browser";

fn stamp() -> String {
    let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) % 86400;
    format!("{:02}:{:02}:{:02}Z", s / 3600, s / 60 % 60, s % 60)
}

fn fail(m: String) -> ! {
    eprintln!("{m}\n\n{USAGE}");
    std::process::exit(2)
}

fn main() {
    let mut opt = Options {
        device_dir: device::default_dir(),
        server: None,
        tls_port: DEFAULT_TLS_PORT,
        plain_port: DEFAULT_PLAIN_PORT,
        ca_file: None,
        log: Arc::new(|line| println!("{} {line}", stamp())),
    };
    let mut args = std::env::args().skip(1).peekable();
    let command = match args.peek().map(String::as_str) {
        Some("run" | "signin" | "status" | "signout" | "recover") => args.next().unwrap_or_default(),
        _ => "run".to_string(),
    };
    let (mut portal, mut method, mut open) = (None, signin::default_method(), true);
    while let Some(a) = args.next() {
        let mut value = || args.next().unwrap_or_else(|| fail(format!("{a} needs a value")));
        let port = |v: String| v.parse::<u16>().unwrap_or_else(|_| fail(format!("{v} is not a port")));
        match a.as_str() {
            "--home" => opt.device_dir = PathBuf::from(value()),
            "--server" => opt.server = Some(value()),
            "--tls-port" => opt.tls_port = port(value()),
            "--plain-port" => opt.plain_port = port(value()),
            "--ca-out" => opt.ca_file = Some(PathBuf::from(value())),
            "--portal" => portal = Some(value()),
            "--method" => {
                let v = value();
                method = Method::parse(&v).unwrap_or_else(|| fail(format!("--method is browser or phone, not {v}")))
            }
            "--no-open" => open = false,
            "-h" | "--help" => {
                println!("{USAGE}");
                return;
            }
            _ => fail(format!("unknown argument {a}")),
        }
    }
    match command.as_str() {
        "signin" => sign_in(&portal.unwrap_or_else(|| fail("signin needs --portal".into())), opt.device_dir, method, open),
        "status" => status(&opt.device_dir),
        "recover" => new_passkey(&portal.unwrap_or_else(|| fail("recover needs --portal".into())), &opt.device_dir, open),
        "signout" => match device::remove(&opt.device_dir) {
            Ok(()) => println!("Signed out on this device. To revoke it, remove the device on the account page too."),
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1)
            }
        },
        _ => run(opt),
    }
}

fn run(opt: Options) {
    let gw = match start(opt) {
        Ok(g) => g,
        Err(e) => {
            eprintln!("The gateway did not start: {e}");
            std::process::exit(1);
        }
    };
    println!("pkp.ini [net]: cloud_ca = {}", gw.ca_file.display());
    println!("pkp.ini [net]: cloud_account_tls_port = {}", gw.tls_port);
    println!("pkp.ini [net]: cloud_account_plain_port = {}", gw.plain_port);
    loop {
        std::thread::park();
    }
}

fn status(dir: &std::path::Path) {
    match device::summary(dir) {
        Ok(s) => {
            let who = if s.account_name.is_empty() { s.account.clone() } else { s.account_name.clone() };
            let state = if signin::expired(s.not_after) { "expired: sign in again" } else { "signed in" };
            println!("{state} as {who} (account {}, device {}, portal {})", s.account, s.device, s.portal);
        }
        Err(_) => println!("signed out"),
    }
}

fn new_passkey(portal: &str, dir: &std::path::Path, open: bool) {
    match recover::link(portal, dir) {
        Ok(None) => println!("This device's account needs no new passkey."),
        Ok(Some(url)) if open => {
            println!("{} opening the page for the new passkey in the browser", stamp());
            if let Err(e) = open_browser(&url) {
                fail(e)
            }
        }
        // the address is the proof itself: for this PC's own browser only
        Ok(Some(url)) => println!("{} open this address on this PC to make the new passkey (it works once, for a short time):
{url}", stamp()),
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1)
        }
    }
}

fn sign_in(portal: &str, dir: PathBuf, method: Method, open: bool) {
    let opener: signin::Opener = if open {
        Box::new(|url: &str| {
            println!("{} opening the sign-in page in the browser", stamp());
            open_browser(url)
        })
    } else {
        Box::new(|url: &str| {
            println!("{} open this address to sign in: {url}", stamp());
            Ok(())
        })
    };
    let flow = signin::Flow::start(portal, dir, method, opener).unwrap_or_else(|e| fail(e));
    let mut said = String::new();
    loop {
        let phase = flow.phase();
        let line = match &phase {
            Phase::Starting => String::new(),
            Phase::Browser { fingerprint, .. } => format!("waiting for the approval in the browser (key {fingerprint})"),
            Phase::Phone { url, user_code, fingerprint, .. } => {
                let qr = signin::qr_text(url).unwrap_or_default();
                format!("on your phone, scan this or open {url}\n{qr}then sign in and type the code {user_code} (key {fingerprint})")
            }
            Phase::Finishing => "approved; fetching this device's certificate".into(),
            Phase::Done(s) => format!("signed in as {} (account {}, device {})", if s.account_name.is_empty() { &s.account } else { &s.account_name }, s.account, s.device),
            Phase::Failed(e) => format!("sign-in failed: {e}"),
            Phase::Cancelled => "sign-in cancelled".into(),
        };
        if line != said && !line.is_empty() {
            println!("{} {line}", stamp());
            said = line;
        }
        if phase.ended() {
            std::process::exit(if matches!(phase, Phase::Done(_)) { 0 } else { 1 });
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
}

/// The default browser, without a shell (the URL is never parsed as a command).
fn open_browser(url: &str) -> Result<(), String> {
    let mut cmd = if cfg!(windows) {
        let mut c = std::process::Command::new("rundll32.exe");
        c.arg("url.dll,FileProtocolHandler");
        c
    } else if cfg!(target_os = "macos") {
        std::process::Command::new("open")
    } else {
        std::process::Command::new("xdg-open")
    };
    cmd.arg(url).spawn().map(|_| ()).map_err(|e| format!("could not open a browser ({e}); open the address by hand"))
}
