// SPDX-License-Identifier: GPL-3.0-or-later
//! Signing this device in to an online account portal, in one of two ways:
//!
//! - **this PC** (`Method::Browser`, the default on Windows): the portal's link page opens in the default browser on
//!   this machine; after a passkey and Approve it sends a one-time code back to a loopback port (RFC 8252,
//!   `http://127.0.0.1:<port>/callback`), checked against a random state. The same flow as the server author's
//!   launcher.py `link`.
//! - **a phone** (`Method::Phone`, the default elsewhere, for handhelds and anything without a handy browser): the
//!   portal parks the request (`/api/link/remote/begin`) and answers a ticket, a poll secret and a short code. The
//!   ticket's page is shown as a QR code with the short code beside it; on the phone the user signs in with a passkey,
//!   sees the device's name and key fingerprint, types the short code and approves. This device polls
//!   (`/api/link/remote/poll`, with the poll secret, which never leaves it) until the one-time code arrives.
//!
//! Either way the one-time code is redeemed at `/api/link/token` with the PKCE verifier and the certificate request,
//! which the approval was bound to (only their hashes were sent before), and the portal answers this device's
//! certificate. The P-256 key is made here and never leaves the device folder (device.rs `save`).
//!
//! The device's name is generic ("Windows PC", "Linux PC", "Mac", "Handheld"), never the computer's own name.
//! Nothing secret is logged: the poll secret, the PKCE verifier, the one-time code and the key stay in memory.

use crate::device;
use base64::engine::general_purpose::URL_SAFE_NO_PAD as B64URL;
use base64::Engine;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};

/// How long the browser on this PC has for the approval.
const BROWSER_SECONDS: u64 = 600;
const HTTP_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Method {
    /// The portal opens in this PC's browser and answers on a loopback port.
    Browser,
    /// A QR code and a short code; approved on a phone, collected by polling.
    Phone,
}

impl Method {
    pub fn parse(s: &str) -> Option<Method> {
        match s {
            "browser" | "pc" => Some(Method::Browser),
            "phone" | "remote" => Some(Method::Phone),
            _ => None,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Method::Browser => "browser",
            Method::Phone => "phone",
        }
    }
}

/// This PC's browser on Windows; a phone on Linux, macOS and handhelds.
pub fn default_method() -> Method {
    if cfg!(windows) {
        Method::Browser
    } else {
        Method::Phone
    }
}

/// The name the portal shows for this device. Generic on purpose: never the computer's own name.
pub fn device_name() -> &'static str {
    if cfg!(windows) {
        "Windows PC"
    } else if cfg!(target_os = "macos") {
        "Mac"
    } else if handheld() {
        "Handheld"
    } else {
        "Linux PC"
    }
}

fn handheld() -> bool {
    if std::env::var_os("SteamDeck").is_some() {
        return true;
    }
    let first_line = |f: &str| std::fs::read_to_string(f).map(|v| v.lines().next().unwrap_or("").trim().to_string()).unwrap_or_default();
    first_line("/sys/class/dmi/id/board_vendor") == "Valve"
        || first_line("/sys/class/dmi/id/sys_vendor") == "Valve"
        || std::fs::read_to_string("/etc/os-release").map(|t| t.lines().any(|l| l.trim() == "ID=steamos")).unwrap_or(false)
}

/// The portal's address, checked: `https://host[:port]`, or `http://` for this machine only (localhost, 127.0.0.1,
/// [::1]: a portal on the same PC, for testing). No path, query or fragment; a trailing slash is dropped.
pub fn check_portal(portal: &str) -> Result<String, String> {
    let p = portal.trim().trim_end_matches('/');
    let bad = || format!("the portal address must look like https://accounts.example.org, not \"{p}\"");
    let (scheme, rest) = p.split_once("://").ok_or_else(bad)?;
    if rest.is_empty() || rest.contains(['/', '?', '#', '@', ' ', '\\']) {
        return Err(bad());
    }
    let host = if let Some(v6) = rest.strip_prefix('[') { format!("[{}]", v6.split(']').next().unwrap_or("")) } else { rest.split(':').next().unwrap_or("").to_string() };
    if host.is_empty() {
        return Err(bad());
    }
    let local = matches!(host.to_ascii_lowercase().as_str(), "localhost" | "127.0.0.1" | "[::1]");
    match scheme.to_ascii_lowercase().as_str() {
        "https" => Ok(p.to_string()),
        "http" if local => Ok(p.to_string()),
        "http" => Err(format!("the portal address must start with https:// (plain http is allowed for this machine only), not \"{p}\"")),
        _ => Err(bad()),
    }
}

fn random(n: usize) -> Result<Vec<u8>, String> {
    let mut b = vec![0u8; n];
    ring::rand::SecureRandom::fill(&ring::rand::SystemRandom::new(), &mut b).map_err(|_| "no random numbers from the system".to_string())?;
    Ok(b)
}

fn sha256(data: &[u8]) -> Vec<u8> {
    ring::digest::digest(&ring::digest::SHA256, data).as_ref().to_vec()
}

fn hex(data: &[u8]) -> String {
    data.iter().map(|b| format!("{b:02x}")).collect()
}

fn same(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// A new key and what the portal is told about it.
struct Request {
    key: rcgen::KeyPair,
    csr_pem: String,
    csr_sha256: String,
    verifier: String,
    challenge: String,
}

impl Request {
    fn new() -> Result<Request, String> {
        let key = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256).map_err(|e| format!("could not make a key: {e}"))?;
        let mut params = rcgen::CertificateParams::new(Vec::<String>::new()).map_err(|e| e.to_string())?;
        params.distinguished_name = rcgen::DistinguishedName::new();
        params.distinguished_name.push(rcgen::DnType::CommonName, "recompia-launcher");
        let csr = params.serialize_request(&key).map_err(|e| format!("could not make the certificate request: {e}"))?;
        let der = csr.der().to_vec();
        let verifier = B64URL.encode(random(48)?);   // 64 characters, RFC 7636
        let challenge = B64URL.encode(sha256(verifier.as_bytes()));
        Ok(Request { csr_pem: pem("CERTIFICATE REQUEST", &der), csr_sha256: hex(&sha256(&der)), key, verifier, challenge })
    }

    /// What the portal's page shows for this request: the first 16 hex digits of the CSR hash, in fours.
    fn fingerprint(&self) -> String {
        fingerprint(&self.csr_sha256)
    }
}

pub fn fingerprint(csr_sha256: &str) -> String {
    csr_sha256.get(..16).unwrap_or("").to_ascii_uppercase().as_bytes().chunks(4).map(|c| String::from_utf8_lossy(c).into_owned()).collect::<Vec<_>>().join(" ")
}

fn pem(label: &str, der: &[u8]) -> String {
    let body = base64::engine::general_purpose::STANDARD.encode(der);
    let lines: Vec<&str> = body.as_bytes().chunks(64).map(|c| std::str::from_utf8(c).unwrap_or("")).collect();
    format!("-----BEGIN {label}-----\n{}\n-----END {label}-----\n", lines.join("\n"))
}

/// Query-string encoding: unreserved characters stay, everything else is %XX.
fn enc(s: &str) -> String {
    s.bytes().map(|b| if b.is_ascii_alphanumeric() || b"-._~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect()
}

fn dec(s: &str) -> Option<String> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'%' => {
                let h = std::str::from_utf8(b.get(i + 1..i + 3)?).ok()?;
                out.push(u8::from_str_radix(h, 16).ok()?);
                i += 3;
            }
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    String::from_utf8(out).ok()
}

// --- the portal's JSON API ------------------------------------------------------------------------------------------

struct Portal {
    base: String,
    agent: ureq::Agent,
}

/// A refusal with the portal's own words (`{"error": ...}`), or a connection failure.
#[derive(Debug)]
enum Fault {
    Status(u16, String),
    Network(String),
}

impl Fault {
    fn text(&self) -> String {
        match self {
            Fault::Status(_, m) => m.clone(),
            Fault::Network(m) => m.clone(),
        }
    }
}

impl Portal {
    fn new(base: &str) -> Result<Portal, String> {
        // certificates are checked against the Mozilla roots ureq carries (webpki-roots); redirects are not followed
        let agent = ureq::AgentBuilder::new().timeout(HTTP_TIMEOUT).redirects(0).user_agent("recompia-launcher").build();
        Ok(Portal { base: check_portal(base)?, agent })
    }

    fn post(&self, path: &str, body: &serde_json::Value) -> Result<serde_json::Value, Fault> {
        let answer = self.agent.post(&format!("{}{path}", self.base)).set("Content-Type", "application/json").send_string(&body.to_string());
        let (status, text) = match answer {
            Ok(r) => (r.status(), r.into_string().map_err(|e| Fault::Network(format!("the portal's answer was cut off: {e}")))?),
            Err(ureq::Error::Status(code, r)) => (code, r.into_string().unwrap_or_default()),
            Err(e) => return Err(Fault::Network(format!("could not reach the portal: {e}"))),
        };
        let json: serde_json::Value = serde_json::from_str(&text).unwrap_or(serde_json::Value::Null);
        if status == 200 {
            return if json.is_object() { Ok(json) } else { Err(Fault::Status(status, "the portal sent an unusable answer".into())) };
        }
        let said = json.get("error").and_then(|e| e.as_str()).map(|s| s.chars().take(300).collect::<String>());
        Err(Fault::Status(status, said.unwrap_or_else(|| format!("the portal answered {status}"))))
    }
}

// --- what a sign-in is doing, for the launcher's dialog -------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Phase {
    Starting,
    /// Waiting for the approval in this PC's browser. `url` is the page that was opened (to open it again).
    Browser { url: String, fingerprint: String },
    /// Waiting for the approval on a phone: the QR code is `url`; the short code is typed there.
    Phone { url: String, user_code: String, qr_svg: String, fingerprint: String, expires_in: u64 },
    /// Approved; fetching the certificate.
    Finishing,
    Done(device::Summary),
    Failed(String),
    Cancelled,
}

impl Phase {
    pub fn ended(&self) -> bool {
        matches!(self, Phase::Done(_) | Phase::Failed(_) | Phase::Cancelled)
    }

    /// The phase for the launcher's front end.
    pub fn to_json(&self) -> serde_json::Value {
        use serde_json::json;
        match self {
            Phase::Starting => json!({ "phase": "starting" }),
            Phase::Browser { url, fingerprint } => json!({ "phase": "browser", "url": url, "fingerprint": fingerprint }),
            Phase::Phone { url, user_code, qr_svg, fingerprint, expires_in } => {
                json!({ "phase": "phone", "url": url, "user_code": user_code, "qr_svg": qr_svg, "fingerprint": fingerprint, "expires_in": expires_in })
            }
            Phase::Finishing => json!({ "phase": "finishing" }),
            Phase::Done(s) => json!({ "phase": "done", "account_name": s.account_name, "account": s.account }),
            Phase::Failed(e) => json!({ "phase": "failed", "error": e }),
            Phase::Cancelled => json!({ "phase": "cancelled" }),
        }
    }
}

/// One sign-in, running on a thread of its own. Dropping it cancels it.
pub struct Flow {
    pub method: Method,
    phase: Arc<Mutex<Phase>>,
    cancel: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

/// Opens a URL in this PC's default browser (the launcher's own opener; the standalone program's prints it).
pub type Opener = Box<dyn Fn(&str) -> Result<(), String> + Send>;

impl Flow {
    /// Starts signing in to `portal`; the new sign-in is written to `dir` when it is approved, replacing any other.
    pub fn start(portal: &str, dir: PathBuf, method: Method, open: Opener) -> Result<Flow, String> {
        let portal = Portal::new(portal)?;
        let phase = Arc::new(Mutex::new(Phase::Starting));
        let cancel = Arc::new(AtomicBool::new(false));
        let (p, c) = (phase.clone(), cancel.clone());
        let thread = std::thread::Builder::new()
            .name("cloud-signin".into())
            .spawn(move || {
                let set = |ph: Phase| *p.lock().unwrap_or_else(|e| e.into_inner()) = ph;
                let result = run(&portal, &dir, method, &open, &set, &c);
                set(match result {
                    _ if c.load(Ordering::Relaxed) => Phase::Cancelled,
                    Ok(summary) => Phase::Done(summary),
                    Err(e) => Phase::Failed(e),
                });
            })
            .map_err(|e| e.to_string())?;
        Ok(Flow { method, phase, cancel, thread: Some(thread) })
    }

    pub fn phase(&self) -> Phase {
        self.phase.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Stops waiting; nothing is written. Returns at once (the thread notices within a quarter second).
    pub fn cancel(&self) {
        self.cancel.store(true, Ordering::Relaxed);
    }

    /// Waits for the end (the standalone program).
    pub fn wait(mut self) -> Phase {
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
        self.phase()
    }
}

impl Drop for Flow {
    fn drop(&mut self) {
        self.cancel();
    }
}

/// Sleeps `d` unless cancelled first; true when cancelled.
fn nap(d: Duration, cancel: &AtomicBool) -> bool {
    let until = Instant::now() + d;
    while Instant::now() < until {
        if cancel.load(Ordering::Relaxed) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(100).min(until.saturating_duration_since(Instant::now())));
    }
    cancel.load(Ordering::Relaxed)
}

fn run(portal: &Portal, dir: &std::path::Path, method: Method, open: &Opener, set: &dyn Fn(Phase), cancel: &AtomicBool) -> Result<device::Summary, String> {
    let req = Request::new()?;
    let code = match method {
        Method::Browser => by_browser(portal, &req, open, set, cancel)?,
        Method::Phone => by_phone(portal, &req, set, cancel)?,
    };
    if cancel.load(Ordering::Relaxed) {
        return Err("cancelled".into());
    }
    set(Phase::Finishing);
    redeem(portal, &req, &code, dir)
}

// --- this PC: the loopback redirect ---------------------------------------------------------------------------------

fn by_browser(portal: &Portal, req: &Request, open: &Opener, set: &dyn Fn(Phase), cancel: &AtomicBool) -> Result<String, String> {
    let listener = TcpListener::bind(SocketAddr::from((Ipv4Addr::LOCALHOST, 0))).map_err(|e| format!("cannot open a loopback port: {e}"))?;
    listener.set_nonblocking(true).map_err(|e| e.to_string())?;
    let port = listener.local_addr().map_err(|e| e.to_string())?.port();
    let state = B64URL.encode(random(24)?);
    let url = format!(
        "{}/link?port={port}&state={state}&challenge={}&csr={}&name={}",
        portal.base,
        req.challenge,
        req.csr_sha256,
        enc(device_name())
    );
    set(Phase::Browser { url: url.clone(), fingerprint: req.fingerprint() });
    open(&url)?;
    let deadline = Instant::now() + Duration::from_secs(BROWSER_SECONDS);
    while Instant::now() < deadline {
        match listener.accept() {
            Ok((stream, _)) => {
                if let Some(code) = callback(stream, &state) {
                    return Ok(code);
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                if nap(Duration::from_millis(150), cancel) {
                    return Err("cancelled".into());
                }
            }
            Err(e) => return Err(format!("the loopback port failed: {e}")),
        }
    }
    Err("No approval arrived from the browser in time. Start again.".into())
}

/// One request to the loopback port: the code when it is the portal's redirect with our state, else None (answered
/// with an error page; the wait goes on).
fn callback(mut stream: TcpStream, state: &str) -> Option<String> {
    let _ = stream.set_nonblocking(false);
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let mut head = Vec::new();
    let mut buf = [0u8; 1024];
    while !head.windows(4).any(|w| w == b"\r\n\r\n") && head.len() < 8192 {
        match stream.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => head.extend_from_slice(&buf[..n]),
        }
    }
    let text = String::from_utf8_lossy(&head);
    let target = text.lines().next().and_then(|l| l.strip_prefix("GET ")).and_then(|l| l.split(' ').next()).unwrap_or("");
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    let mut code = None;
    let mut got_state = None;
    for pair in query.split('&') {
        match pair.split_once('=') {
            Some(("code", v)) => code = dec(v),
            Some(("state", v)) => got_state = dec(v),
            _ => {}
        }
    }
    let good = path == "/callback"
        && got_state.as_deref().map(|s| same(s.as_bytes(), state.as_bytes())).unwrap_or(false)
        && code.as_deref().map(|c| !c.is_empty() && c.len() <= 256).unwrap_or(false);
    let (status, words) = if good {
        ("200 OK", "Approval received. Return to the launcher: it finishes signing in by itself. You can close this tab.")
    } else {
        ("400 Bad Request", "This sign-in request does not belong to the launcher that is waiting.")
    };
    let body = format!("<!doctype html><meta charset=utf-8><title>Online account</title><p style=\"font:16px system-ui;margin:3em\">{words}</p>");
    let _ = write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    let _ = stream.flush();
    if good {
        code
    } else {
        None
    }
}

// --- a phone: the parked request ------------------------------------------------------------------------------------

fn matches_set(s: &str, len: usize, ok: impl Fn(u8) -> bool) -> bool {
    s.len() == len && s.bytes().all(ok)
}

fn b64url_char(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'-' || b == b'_'
}

fn by_phone(portal: &Portal, req: &Request, set: &dyn Fn(Phase), cancel: &AtomicBool) -> Result<String, String> {
    let begun = portal
        .post("/api/link/remote/begin", &serde_json::json!({ "challenge": req.challenge, "csr": req.csr_sha256, "name": device_name() }))
        .map_err(|f| f.text())?;
    let field = |k: &str| begun.get(k).and_then(|v| v.as_str()).unwrap_or("").to_string();
    let (ticket, poll, user_code) = (field("ticket"), field("poll"), field("user_code"));
    let code_ok = user_code.len() == 9 && user_code.bytes().enumerate().all(|(i, b)| if i == 4 { b == b'-' } else { b.is_ascii_uppercase() });
    if !matches_set(&ticket, 32, b64url_char) || !matches_set(&poll, 43, b64url_char) || !code_ok {
        return Err("the portal sent an unusable answer".into());
    }
    let expires_in = begun.get("expires_in").and_then(|v| v.as_u64()).unwrap_or(300).clamp(30, 900);
    let mut interval = begun.get("interval").and_then(|v| v.as_u64()).unwrap_or(3).clamp(1, 60);
    // the QR code's address is made here from the configured portal, not taken from the answer
    let url = format!("{}/link?ticket={ticket}", portal.base);
    let qr = qr_svg(&url)?;
    let deadline = Instant::now() + Duration::from_secs(expires_in);
    let left = || deadline.saturating_duration_since(Instant::now()).as_secs();
    set(Phase::Phone { url: url.clone(), user_code: user_code.clone(), qr_svg: qr.clone(), fingerprint: req.fingerprint(), expires_in: left() });
    let mut network_errors = 0;
    loop {
        if nap(Duration::from_secs(interval), cancel) {
            return Err("cancelled".into());
        }
        if Instant::now() >= deadline {
            return Err("The sign-in request expired. Start again.".into());
        }
        set(Phase::Phone { url: url.clone(), user_code: user_code.clone(), qr_svg: qr.clone(), fingerprint: req.fingerprint(), expires_in: left() });
        match portal.post("/api/link/remote/poll", &serde_json::json!({ "ticket": ticket, "poll": poll })) {
            Ok(answer) => {
                network_errors = 0;
                match answer.get("status").and_then(|s| s.as_str()) {
                    Some("approved") => {
                        let code = answer.get("code").and_then(|c| c.as_str()).unwrap_or("");
                        if code.is_empty() || code.len() > 256 {
                            return Err("the portal sent an unusable answer".into());
                        }
                        return Ok(code.to_string());
                    }
                    Some("pending") => interval = answer.get("interval").and_then(|v| v.as_u64()).unwrap_or(interval).clamp(1, 60),
                    _ => return Err("the portal sent an unusable answer".into()),
                }
            }
            Err(Fault::Status(429, _)) => interval = (interval * 2).min(30),
            Err(Fault::Network(e)) => {
                network_errors += 1;
                if network_errors >= 5 {
                    return Err(e);
                }
            }
            Err(f) => return Err(f.text()),
        }
    }
}

// --- the certificate ------------------------------------------------------------------------------------------------

fn redeem(portal: &Portal, req: &Request, code: &str, dir: &std::path::Path) -> Result<device::Summary, String> {
    let answer = portal
        .post("/api/link/token", &serde_json::json!({ "code": code, "verifier": req.verifier, "csr": req.csr_pem }))
        .map_err(|f| f.text())?;
    let unusable = || "the portal sent an unusable answer".to_string();
    let text = |v: &serde_json::Value, k: &str| v.get(k).and_then(|x| x.as_str()).map(str::to_string).ok_or_else(unusable);
    let ingress = answer.get("ingress").ok_or_else(unusable)?;
    let port = ingress.get("port").and_then(|p| p.as_u64()).filter(|p| (1..=65535).contains(p)).ok_or_else(unusable)? as u16;
    let certificate = text(&answer, "certificate")?;
    let ca = text(ingress, "ca")?;
    use rustls::pki_types::pem::PemObject;
    let cert = rustls::pki_types::CertificateDer::from_pem_slice(certificate.as_bytes()).map_err(|_| unusable())?;
    rustls::pki_types::CertificateDer::from_pem_slice(ca.as_bytes()).map_err(|_| unusable())?;
    let (spki, _) = cert_fields(&cert).ok_or_else(unusable)?;
    if !same(&spki, &req.key.public_key_der()) {
        return Err("The portal issued a certificate for a different key.".into());
    }
    let (account, account_name, dev) = (text(&answer, "account")?, text(&answer, "account_name").unwrap_or_default(), text(&answer, "device")?);
    let (host, server_name) = (text(ingress, "host")?, text(ingress, "server_name")?);
    let key = req.key.serialize_der();
    device::save(
        dir,
        &device::NewDevice {
            portal: &portal.base,
            account: &account,
            account_name: &account_name,
            device: &dev,
            host: &host,
            port,
            server_name: &server_name,
            certificate_pem: &certificate,
            ingress_ca_pem: &ca,
            key_pkcs8: &key,
        },
    )?;
    device::summary(dir)
}

// --- DER: just enough of a certificate to check its key and read its end --------------------------------------------

/// tag, contents, the whole element, what follows it
fn tlv(b: &[u8]) -> Option<(u8, &[u8], &[u8], &[u8])> {
    let (&tag, rest) = b.split_first()?;
    let (&first, rest) = rest.split_first()?;
    let (len, rest) = if first < 0x80 {
        (first as usize, rest)
    } else {
        let n = (first & 0x7f) as usize;
        if n == 0 || n > 4 || rest.len() < n {
            return None;
        }
        (rest[..n].iter().fold(0usize, |a, &x| (a << 8) | x as usize), &rest[n..])
    };
    if rest.len() < len {
        return None;
    }
    let header = b.len() - rest.len();
    Some((tag, &rest[..len], &b[..header + len], &rest[len..]))
}

fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let doy = (153 * (m + if m > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    era * 146_097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719_468
}

fn der_time(tag: u8, v: &[u8]) -> Option<i64> {
    let s = std::str::from_utf8(v).ok()?.strip_suffix('Z')?;
    let (year, rest) = match tag {
        0x17 if s.len() == 12 => {
            let yy: i64 = s[..2].parse().ok()?;
            (if yy >= 50 { 1900 + yy } else { 2000 + yy }, &s[2..])
        }
        0x18 if s.len() == 14 => (s[..4].parse().ok()?, &s[4..]),
        _ => return None,
    };
    let n = |i: usize| -> Option<i64> { rest.get(i..i + 2)?.parse().ok() };
    let (mo, d, h, mi, se) = (n(0)?, n(2)?, n(4)?, n(6)?, n(8)?);
    if !(1..=12).contains(&mo) || !(1..=31).contains(&d) || h > 23 || mi > 59 || se > 60 {
        return None;
    }
    Some(days_from_civil(year, mo, d) * 86_400 + h * 3600 + mi * 60 + se)
}

/// A certificate's SubjectPublicKeyInfo (DER) and notAfter (Unix seconds).
pub fn cert_fields(der: &[u8]) -> Option<(Vec<u8>, i64)> {
    let (0x30, cert, _, _) = tlv(der)? else { return None };
    let (0x30, tbs, _, _) = tlv(cert)? else { return None };
    let mut rest = tbs;
    let (tag, _, _, r) = tlv(rest)?;
    if tag == 0xa0 {
        rest = r; // version
    }
    let (0x02, _, _, r) = tlv(rest)? else { return None }; // serial
    let (0x30, _, _, r) = tlv(r)? else { return None }; // signature algorithm
    let (0x30, _, _, r) = tlv(r)? else { return None }; // issuer
    let (0x30, validity, _, r) = tlv(r)? else { return None };
    let (0x30, _, _, r) = tlv(r)? else { return None }; // subject
    let (0x30, _, spki, _) = tlv(r)? else { return None };
    let (_, _, _, after) = tlv(validity)?;
    let (tag, end, _, _) = tlv(after)?;
    Some((spki.to_vec(), der_time(tag, end)?))
}

/// Whether a certificate that ends at `not_after` has ended.
pub fn expired(not_after: i64) -> bool {
    let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    not_after <= now
}

// --- the QR code ----------------------------------------------------------------------------------------------------

fn qr(text: &str, ecc: qrcodegen::QrCodeEcc) -> Result<qrcodegen::QrCode, String> {
    qrcodegen::QrCode::encode_text(text, ecc).map_err(|_| "the sign-in address is too long for a QR code".to_string())
}

/// The light square kept in the middle of a QR code `n` modules wide for the launcher's mark: (first module, side). About a fifth
/// of the code's width (an odd number of modules, so it sits on the centre module).
pub fn qr_badge(n: i32) -> (i32, i32) {
    let side = 2 * (n / 10) + 1;
    ((n - side) / 2, side)
}

/// Screen pixels per QR module: whole pixels keep every module the same width on screen (at 100 % to 175 % display scale), which
/// is what keeps the code readable when a phone photographs it from further away.
pub const QR_PX_PER_MODULE: i32 = 4;

/// The QR code of `text` as a small SVG: black modules on white with the standard four-module margin, at level H (30 % of the
/// code may be lost) with the middle square (`qr_badge`) left light and marked by a last white `<rect>`, where the launcher draws
/// the mark; `QR_PX_PER_MODULE` pixels a module. With the mark drawn in, rendered dialogs decoded (rqrr and jsQR) as shown and
/// downscaled like a phone photo, the same as the code without the mark.
pub fn qr_svg(text: &str) -> Result<String, String> {
    let code = qr(text, qrcodegen::QrCodeEcc::High)?;
    let n = code.size();
    let (b, bs) = qr_badge(n);
    let in_badge = |x: i32, y: i32| x >= b && y >= b && x < b + bs && y < b + bs;
    let mut path = String::new();
    for y in 0..n {
        for x in 0..n {
            if code.get_module(x, y) && !in_badge(x, y) {
                path.push_str(&format!("M{},{}h1v1h-1z", x + 4, y + 4));
            }
        }
    }
    let side = n + 8;
    Ok(format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{px}\" height=\"{px}\" viewBox=\"0 0 {side} {side}\" shape-rendering=\"crispEdges\"><rect width=\"{side}\" height=\"{side}\" fill=\"#fff\"/><path d=\"{path}\" fill=\"#000\"/><rect x=\"{x}\" y=\"{x}\" width=\"{bs}\" height=\"{bs}\" fill=\"#fff\"/></svg>",
        x = b + 4,
        px = side * QR_PX_PER_MODULE
    ))
}

/// The QR code of `text` for a terminal (the standalone program on a handheld): two rows per line in half blocks.
pub fn qr_text(text: &str) -> Result<String, String> {
    let code = qr(text, qrcodegen::QrCodeEcc::Medium)?;
    let n = code.size();
    let dark = |x: i32, y: i32| x >= 0 && y >= 0 && x < n && y < n && code.get_module(x, y);
    let mut out = String::new();
    let mut y = -2;
    while y < n + 2 {
        for x in -2..n + 2 {
            // light on dark terminals and dark on light ones alike: draw the light modules
            out.push(match (!dark(x, y), !dark(x, y + 1)) {
                (true, true) => '█',
                (true, false) => '▀',
                (false, true) => '▄',
                (false, false) => ' ',
            });
        }
        out.push('\n');
        y += 2;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn portal_addresses() {
        assert_eq!(check_portal("https://accounts.example.org/").unwrap(), "https://accounts.example.org");
        assert_eq!(check_portal("http://localhost:8790").unwrap(), "http://localhost:8790");
        assert_eq!(check_portal("http://127.0.0.1:8790").unwrap(), "http://127.0.0.1:8790");
        for bad in ["http://accounts.example.org", "ftp://x", "https://", "https://a/b", "https://a?x", "accounts.example.org", "https://u@a", "http://localhost.example.org"] {
            assert!(check_portal(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn request_is_pkce_and_p256() {
        let r = Request::new().unwrap();
        assert_eq!(r.verifier.len(), 64);
        assert!(r.verifier.bytes().all(b64url_char));
        assert_eq!(r.challenge.len(), 43);
        assert_eq!(r.challenge, B64URL.encode(sha256(r.verifier.as_bytes())));
        assert_eq!(r.csr_sha256.len(), 64);
        assert!(r.csr_pem.starts_with("-----BEGIN CERTIFICATE REQUEST-----\n"));
        assert_eq!(r.fingerprint().len(), 19);
        // the key is P-256 (id-ecPublicKey with prime256v1)
        let spki = r.key.public_key_der();
        let p256 = [0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07];
        assert!(spki.windows(p256.len()).any(|w| w == p256));
    }

    #[test]
    fn reads_a_certificate() {
        let key = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256).unwrap();
        let mut params = rcgen::CertificateParams::new(vec!["localhost".to_string()]).unwrap();
        params.not_after = rcgen::date_time_ymd(2031, 2, 3);
        let cert = params.self_signed(&key).unwrap();
        let (spki, end) = cert_fields(cert.der()).unwrap();
        assert_eq!(spki, key.public_key_der());
        assert_eq!(end, 1_927_843_200); // 2031-02-03T00:00:00Z
        assert!(!expired(end));
        assert!(cert_fields(&cert.der()[..40]).is_none());
    }

    #[test]
    fn callback_query() {
        assert_eq!(enc("Windows PC"), "Windows%20PC");
        assert_eq!(dec("a%2Fb+c").as_deref(), Some("a/b c"));
        assert_eq!(dec("%zz"), None);
        assert_eq!(fingerprint("0123456789abcdef0000"), "0123 4567 89AB CDEF");
    }

    #[test]
    fn loopback_callback_checks_the_state() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let ask = |target: &'static str| {
            std::thread::spawn(move || {
                let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
                write!(s, "GET {target} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").unwrap();
                let mut answer = String::new();
                let _ = s.read_to_string(&mut answer);
                answer
            })
        };
        let wrong = ask("/callback?code=abc&state=other-state-value");
        assert_eq!(callback(listener.accept().unwrap().0, "the-right-state-1"), None);
        assert!(wrong.join().unwrap().starts_with("HTTP/1.1 400"));
        let right = ask("/callback?code=abc&state=the-right-state-1");
        assert_eq!(callback(listener.accept().unwrap().0, "the-right-state-1").as_deref(), Some("abc"));
        assert!(right.join().unwrap().starts_with("HTTP/1.1 200"));
    }

    #[test]
    fn qr_codes() {
        let url = "http://127.0.0.1:8790/link?ticket=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        let svg = qr_svg(url).unwrap();
        assert!(svg.starts_with("<svg ") && svg.contains("<path d=\"M"));
        let code = qr(url, qrcodegen::QrCodeEcc::High).unwrap();
        assert_eq!(code.error_correction_level(), qrcodegen::QrCodeEcc::High);
        let n = code.size();
        let px = (n + 8) * QR_PX_PER_MODULE;
        assert!(svg.contains(&format!(" width=\"{px}\" height=\"{px}\" viewBox=\"0 0 {0} {0}\"", n + 8)));
        let (b, bs) = qr_badge(n);
        assert!(bs % 2 == 1 && b * 2 + bs == n && bs * 100 <= n * 22, "badge {bs} of {n}");
        assert!(svg.ends_with(&format!("<rect x=\"{0}\" y=\"{0}\" width=\"{bs}\" height=\"{bs}\" fill=\"#fff\"/></svg>", b + 4)));
        // no dark module drawn in the badge
        for y in b..b + bs {
            for x in b..b + bs {
                assert!(!svg.contains(&format!("M{},{}h", x + 4, y + 4)));
            }
        }
        assert!(qr_text("x").unwrap().lines().count() > 10);
    }

    /// Saves a device signed in with a certificate from a throwaway authority (its server: 127.0.0.1:9443, localhost).
    fn save_test_device(dir: &std::path::Path, account: &str, device: &str) -> Vec<u8> {
        let req = Request::new().unwrap();
        // a certificate for the request's key, from a throwaway authority
        let ca_key = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256).unwrap();
        let mut ca_params = rcgen::CertificateParams::new(Vec::<String>::new()).unwrap();
        ca_params.is_ca = rcgen::IsCa::Ca(rcgen::BasicConstraints::Unconstrained);
        let ca = ca_params.self_signed(&ca_key).unwrap();
        let leaf = rcgen::CertificateParams::new(Vec::<String>::new()).unwrap().signed_by(&req.key, &ca, &ca_key).unwrap();
        let key = req.key.serialize_der();
        let (cert_pem, ca_pem) = (pem("CERTIFICATE", leaf.der()), pem("CERTIFICATE", ca.der()));
        let new = device::NewDevice {
            portal: "http://127.0.0.1:8790",
            account,
            account_name: "Test",
            device,
            host: "127.0.0.1",
            port: 9443,
            server_name: "localhost",
            certificate_pem: &cert_pem,
            ingress_ca_pem: &ca_pem,
            key_pkcs8: &key,
        };
        device::save(dir, &new).unwrap();
        key
    }

    #[test]
    fn saves_loads_and_removes_a_sign_in() {
        let dir = std::env::temp_dir().join(format!("recompia-signin-test-{}", hex(&random(6).unwrap())));
        let key = save_test_device(&dir, "acct-test", "dev-test");
        assert!(device::linked(&dir));
        let loaded = device::load(&dir).unwrap();
        assert_eq!((loaded.account.as_str(), loaded.device.as_str(), loaded.port), ("acct-test", "dev-test", 9443));
        assert_eq!(loaded.key.secret_der(), &key[..]);
        let text = std::fs::read_to_string(dir.join("device.key")).unwrap();
        assert_eq!(text.contains("ENCRYPTED PRIVATE KEY"), cfg!(windows));
        assert_eq!(dir.join("device.key.dpapi").is_file(), cfg!(windows));
        let s = device::summary(&dir).unwrap();
        assert_eq!((s.account_name.as_str(), s.device.as_str()), ("Test", "dev-test"));
        device::remove(&dir).unwrap();
        assert!(!device::linked(&dir));
        assert!(device::FILES.iter().all(|f| !dir.join(f).exists()));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_gateway_log_names_ids_by_their_first_4_characters() {
        let dir = std::env::temp_dir().join(format!("recompia-signin-test-{}", hex(&random(6).unwrap())));
        save_test_device(&dir, "acct-abcd2345efgh", "dev-wxyz6789mnop");
        let lines = std::sync::Arc::new(std::sync::Mutex::new(Vec::<String>::new()));
        let sink = lines.clone();
        let linked = crate::Linked { dir: dir.clone(), log: std::sync::Arc::new(move |l: &str| sink.lock().unwrap().push(l.to_string())), seen: std::sync::Mutex::new(None) };
        assert!(linked.upstream().is_ok());
        let said = lines.lock().unwrap().join("\n");
        assert_eq!(said, "gateway: signed in as device dev-wxyz.. of account acct-abcd.. -> 127.0.0.1:9443 (localhost)");
        device::remove(&dir).unwrap();
        let _ = std::fs::remove_dir_all(&dir);
    }
}
