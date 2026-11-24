// SPDX-License-Identifier: GPL-3.0-or-later
//! Cloud Islands account gateway.
//!
//! A Cloud Islands server in account mode takes only TLS connections that present a device certificate its portal
//! issued. The game cannot present one, so this gateway stands between them, on this machine only:
//!
//! - a TLS listener on 127.0.0.1 for the game's own TLS connections (gRPC). Its certificate is made at start
//!   (certs.rs) and the game trusts it through `[net] cloud_ca`;
//! - a plaintext listener on 127.0.0.1 for the island file, which the runtime hands over without TLS;
//! - for each connection, one connection to the server over mutual TLS: the device certificate and key from the
//!   device folder (device.rs), the server's certificate checked against the portal's ingress authority and nothing
//!   else, and the same application protocol (ALPN) the game chose. Bytes are passed unchanged both ways.
//!
//! Anyone who can connect to the listeners acts as this device's account, so they are never bound to anything but
//! 127.0.0.1. One log line per connection: counts and how it ended, never contents. Players post the log when they report a
//! problem, so it names the account and the device by the first 4 characters of their ids only (`short_id`).
//!
//! The device folder is looked at again for every connection (sizes and times of its files only; it is read again
//! when they changed), so a device signed in, signed in again or signed out while the game runs is used from the next
//! connection on, without restarting the game. Signed out, connections are closed at once.
//!
//! Signing a device in (signin.rs) lives here too, so the launcher and the standalone program share one implementation.

pub mod certs;
pub mod device;
pub mod signin;

use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName};
use rustls::{ClientConfig, RootCertStore, ServerConfig};
use std::net::{Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio_rustls::{TlsAcceptor, TlsConnector};

pub const DEFAULT_TLS_PORT: u16 = 18443;
pub const DEFAULT_PLAIN_PORT: u16 = 18444;
/// The file the gateway's authority certificate is written to in the device folder, for `[net] cloud_ca`.
pub const CA_FILE: &str = "recompia-gateway-ca.der";

const HANDSHAKE: Duration = Duration::from_secs(15);
const CONNECT: Duration = Duration::from_secs(15);
/// After the game closes its side, how long the server may still answer.
const GRACE: Duration = Duration::from_secs(10);
const CHUNK: usize = 64 * 1024;
const MAX_CONNECTIONS: usize = 256;

pub type Log = Arc<dyn Fn(&str) + Send + Sync>;

pub struct Options {
    /// The device folder (device.rs). It need not hold a sign-in yet.
    pub device_dir: PathBuf,
    /// The server's address when the caller knows it: dialled instead of the host the sign-in stored (a name that may
    /// no longer resolve). The port and the name the server's certificate is checked against stay the sign-in's.
    pub server: Option<String>,
    /// 0: any free port (read it back from `Gateway`).
    pub tls_port: u16,
    pub plain_port: u16,
    /// Where to write the authority certificate the game must trust (DER); None: `CA_FILE` in the device folder.
    pub ca_file: Option<PathBuf>,
    pub log: Log,
}

/// A running gateway. Dropping it (or `stop`) closes the listeners and every connection.
pub struct Gateway {
    pub tls_port: u16,
    pub plain_port: u16,
    pub ca_file: PathBuf,
    stop: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Gateway {
    pub fn stop(mut self) {
        self.end();
    }
    fn end(&mut self) {
        drop(self.stop.take());
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for Gateway {
    fn drop(&mut self) {
        self.end();
    }
}

struct Upstream {
    host: String,
    port: u16,
    name: ServerName<'static>,
    h2: TlsConnector,
    http1: TlsConnector,
    none: TlsConnector,
}

impl Upstream {
    fn new(dev: &device::Device, server: Option<&str>) -> Result<Upstream, String> {
        let name = ServerName::try_from(dev.server_name.clone()).map_err(|_| "launcher.json names no usable server_name".to_string())?;
        let connector = |alpn: Option<&[u8]>| client_config(dev, alpn).map(|c| TlsConnector::from(Arc::new(c)));
        Ok(Upstream {
            host: server.unwrap_or(&dev.host).to_string(),
            port: dev.port,
            name,
            h2: connector(Some(b"h2"))?,
            http1: connector(Some(b"http/1.1"))?,
            none: connector(None)?,
        })
    }

    /// One connection to the server, offering exactly the protocol the game chose.
    async fn connect(&self, alpn: Option<&[u8]>) -> Result<tokio_rustls::client::TlsStream<TcpStream>, String> {
        let connector = match alpn {
            Some(b"h2") => &self.h2,
            Some(b"http/1.1") => &self.http1,
            _ => &self.none,
        };
        let work = async {
            let tcp = TcpStream::connect((self.host.as_str(), self.port)).await.map_err(|e| format!("the server does not answer: {e}"))?;
            let _ = tcp.set_nodelay(true);
            connector.connect(self.name.clone(), tcp).await.map_err(|e| format!("the server's TLS handshake failed: {e}"))
        };
        tokio::time::timeout(CONNECT, work).await.unwrap_or_else(|_| Err("the server did not answer in time".into()))
    }
}

fn provider() -> Arc<rustls::crypto::CryptoProvider> {
    Arc::new(rustls::crypto::ring::default_provider())
}

fn client_config(dev: &device::Device, alpn: Option<&[u8]>) -> Result<ClientConfig, String> {
    let mut roots = RootCertStore::empty();
    for c in &dev.ingress_ca {
        roots.add(c.clone()).map_err(|e| format!("ingress-ca.pem is not usable: {e}"))?;
    }
    let mut cfg = ClientConfig::builder_with_provider(provider())
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_root_certificates(roots)
        .with_client_auth_cert(dev.chain.clone(), dev.key.clone_key())
        .map_err(|e| format!("the device certificate and key do not fit: {e}"))?;
    cfg.alpn_protocols = alpn.map(|a| vec![a.to_vec()]).unwrap_or_default();
    Ok(cfg)
}

fn server_config(id: &certs::Identity) -> Result<ServerConfig, String> {
    let chain = vec![CertificateDer::from(id.leaf_der.clone())];
    let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(id.leaf_key.clone()));
    let mut cfg = ServerConfig::builder_with_provider(provider())
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_no_client_auth()
        .with_single_cert(chain, key)
        .map_err(|e| e.to_string())?;
    cfg.alpn_protocols = vec![b"h2".to_vec(), b"http/1.1".to_vec()];
    Ok(cfg)
}

fn bind(port: u16, what: &str) -> Result<std::net::TcpListener, String> {
    let l = std::net::TcpListener::bind(SocketAddr::from((Ipv4Addr::LOCALHOST, port)))
        .map_err(|e| format!("cannot listen on 127.0.0.1:{port} ({what}): {e}"))?;
    l.set_nonblocking(true).map_err(|e| e.to_string())?;
    Ok(l)
}

/// Sizes and modification times of the device files: when they change, the folder is read again.
type Stamp = Vec<Option<(u64, SystemTime)>>;

/// A failed read (not signed in, a file in the middle of being replaced) is tried again after this long even when
/// nothing seems to have changed.
const RETRY: Duration = Duration::from_secs(5);

fn stamp(dir: &Path) -> Stamp {
    device::FILES.iter().map(|f| std::fs::metadata(dir.join(f)).ok().and_then(|m| Some((m.len(), m.modified().ok()?)))).collect()
}

/// The device folder as of the last connection.
struct Linked {
    dir: PathBuf,
    server: Option<String>,
    log: Log,
    seen: Mutex<Option<Seen>>,
}

struct Seen {
    stamp: Stamp,
    at: Instant,
    upstream: Result<Arc<Upstream>, String>,
    said: String,
}

impl Linked {
    /// The server connection settings of the device signed in now; read again when the device files changed.
    fn upstream(&self) -> Result<Arc<Upstream>, String> {
        let now = stamp(&self.dir);
        let mut seen = self.seen.lock().unwrap_or_else(|p| p.into_inner());
        if let Some(s) = seen.as_ref() {
            if s.stamp == now && (s.upstream.is_ok() || s.at.elapsed() < RETRY) {
                return s.upstream.clone();
            }
        }
        let (upstream, said) = if !device::linked(&self.dir) {
            (Err("not signed in: no device in the device folder".to_string()), "gateway: not signed in; connections are closed until a device signs in".to_string())
        } else {
            match device::load(&self.dir).and_then(|d| Upstream::new(&d, self.server.as_deref()).map(|u| (d, u))) {
                Ok((d, u)) => {
                    let said = format!("gateway: signed in as device {} of account {} -> {}:{} ({})", short_id(&d.device), short_id(&d.account), u.host, d.port, d.server_name);
                    (Ok(Arc::new(u)), said)
                }
                Err(e) => (Err(e.clone()), format!("gateway: the device folder is not usable: {e}")),
            }
        };
        if seen.as_ref().map(|s| &s.said) != Some(&said) {
            (self.log)(&said);
        }
        *seen = Some(Seen { stamp: now, at: Instant::now(), upstream: upstream.clone(), said });
        upstream
    }
}

/// An account or device id as the log shows it: its kind (`acct-`, `dev-`) and the first 4 characters after it, then `..` when
/// there were more. Enough to tell two apart in a report; the whole id stays out of a log that may be posted in public.
fn short_id(id: &str) -> String {
    let (kind, rest) = match id.split_once('-') {
        Some((k, r)) if (1..=8).contains(&k.len()) && k.bytes().all(|b| b.is_ascii_lowercase()) => (&id[..=k.len()], r),
        _ => ("", id),
    };
    let head: String = rest.chars().take(4).collect();
    let more = if rest.chars().count() > 4 { ".." } else { "" };
    format!("{kind}{head}{more}")
}

/// Makes the gateway's certificate, opens both listeners and serves on a thread of its own. The device folder is read
/// per connection (see the top of this file), so it need not hold a sign-in yet.
pub fn start(opt: Options) -> Result<Gateway, String> {
    let identity = certs::make().map_err(|e| format!("could not make the gateway certificate: {e}"))?;
    let acceptor = TlsAcceptor::from(Arc::new(server_config(&identity)?));

    let tls = bind(opt.tls_port, "the game's TLS connections")?;
    let plain = bind(opt.plain_port, "the island file")?;
    let port_of = |l: &std::net::TcpListener| l.local_addr().map(|a| a.port()).map_err(|e| e.to_string());
    let (tls_port, plain_port) = (port_of(&tls)?, port_of(&plain)?);

    let ca_file = match opt.ca_file.clone() {
        Some(f) => f,
        None => {
            device::make_dir(&opt.device_dir)?;
            opt.device_dir.join(CA_FILE)
        }
    };
    std::fs::write(&ca_file, &identity.ca_der).map_err(|e| format!("cannot write {}: {e}", ca_file.display()))?;

    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().map_err(|e| e.to_string())?;
    let (stop, stopped) = tokio::sync::oneshot::channel::<()>();
    let log = opt.log.clone();
    log(&format!("gateway: TLS 127.0.0.1:{tls_port}, plain 127.0.0.1:{plain_port}"));
    let linked = Arc::new(Linked { dir: opt.device_dir.clone(), server: opt.server.clone(), log: log.clone(), seen: Mutex::new(None) });
    let _ = linked.upstream();   // says at once whether a device is signed in
    let slots = Arc::new(tokio::sync::Semaphore::new(MAX_CONNECTIONS));
    let thread = std::thread::Builder::new()
        .name("cloud-gateway".into())
        .spawn(move || {
            rt.block_on(async move {
                let serve = serve(tls, plain, acceptor, linked, slots.clone(), log.clone());
                tokio::select! {
                    r = serve => if let Err(e) = r { log(&format!("gateway: stopped: {e}")) },
                    // a connection logs its line when it ends; the ones still open now (the game's long gRPC ones, in their
                    // grace time after the game has gone) end with the runtime below, so the log says how many there were
                    _ = stopped => match MAX_CONNECTIONS - slots.available_permits() {
                        0 => log("gateway: stopped"),
                        n => log(&format!("gateway: stopped; {n} connection(s) still open were closed")),
                    }
                }
            });
            // dropping the runtime here ends every connection task and closes its sockets
        })
        .map_err(|e| e.to_string())?;
    Ok(Gateway { tls_port, plain_port, ca_file, stop: Some(stop), thread: Some(thread) })
}

async fn serve(tls: std::net::TcpListener, plain: std::net::TcpListener, acceptor: TlsAcceptor, linked: Arc<Linked>, slots: Arc<tokio::sync::Semaphore>, log: Log) -> Result<(), String> {
    let tls = TcpListener::from_std(tls).map_err(|e| e.to_string())?;
    let plain = TcpListener::from_std(plain).map_err(|e| e.to_string())?;
    let ids = AtomicU64::new(1);
    loop {
        let (accepted, secure) = tokio::select! {
            a = tls.accept() => (a, true),
            a = plain.accept() => (a, false),
        };
        let stream = match accepted {
            Ok((s, _)) => s,
            Err(e) => {
                log(&format!("gateway: accept failed: {e}"));
                tokio::time::sleep(Duration::from_millis(200)).await;
                continue;
            }
        };
        let Ok(slot) = slots.clone().try_acquire_owned() else { continue };   // full: the connection is dropped
        let _ = stream.set_nodelay(true);
        let id = ids.fetch_add(1, Ordering::Relaxed);
        let (acceptor, log) = (acceptor.clone(), log.clone());
        let upstream = linked.upstream();
        tokio::spawn(async move {
            let started = Instant::now();
            let line = match upstream {
                Err(e) => e,   // dropping the stream closes it before any byte
                Ok(upstream) if secure => from_game_tls(stream, acceptor, &upstream).await,
                Ok(upstream) => from_game_plain(stream, &upstream).await,
            };
            log(&format!("c{id} {} {line} ({:.1} s)", if secure { "tls" } else { "plain" }, started.elapsed().as_secs_f32()));
            drop(slot);
        });
    }
}

/// The game's own TLS (gRPC): finish its handshake with the gateway certificate, then pass its bytes on.
async fn from_game_tls(stream: TcpStream, acceptor: TlsAcceptor, upstream: &Upstream) -> String {
    let game = match tokio::time::timeout(HANDSHAKE, acceptor.accept(stream)).await {
        Ok(Ok(g)) => g,
        // the runtime opens the island file's connection here first and swaps it for the plaintext port before any byte
        Ok(Err(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => return "closed before a handshake".into(),
        Ok(Err(e)) => return format!("the game's TLS handshake failed: {e}"),
        Err(_) => return "no TLS handshake from the game in time".into(),
    };
    let alpn = game.get_ref().1.alpn_protocol().map(<[u8]>::to_vec);
    let label = alpn_label(alpn.as_deref());
    match upstream.connect(alpn.as_deref()).await {
        Ok(server) => format!("alpn={label} {}", pipe(game, server).await),
        Err(e) => format!("alpn={label} {e}"),
    }
}

/// The island file: the runtime's nn::ssl passes the game's bytes without TLS. HTTP/2 starts with a fixed preface;
/// anything else is HTTP/1.1.
async fn from_game_plain(stream: TcpStream, upstream: &Upstream) -> String {
    let mut first = [0u8; 4];
    let alpn: &[u8] = match tokio::time::timeout(HANDSHAKE, stream.peek(&mut first)).await {
        Ok(Ok(0)) => return "closed before any byte".into(),
        Ok(Ok(n)) if first[..n] == b"PRI "[..n] => b"h2",
        Ok(Ok(_)) => b"http/1.1",
        Ok(Err(e)) => return format!("ended: {e}"),
        Err(_) => return "nothing from the game in time".into(),
    };
    let label = alpn_label(Some(alpn));
    match upstream.connect(Some(alpn)).await {
        Ok(server) => format!("alpn={label} {}", pipe(stream, server).await),
        Err(e) => format!("alpn={label} {e}"),
    }
}

fn alpn_label(a: Option<&[u8]>) -> &'static str {
    match a {
        Some(b"h2") => "h2",
        Some(b"http/1.1") => "http/1.1",
        Some(_) => "other",
        None => "none",
    }
}

async fn pump<R: AsyncRead + Unpin, W: AsyncWrite + Unpin>(mut from: R, mut to: W, count: &AtomicU64) -> std::io::Result<()> {
    let mut buf = vec![0u8; CHUNK];
    loop {
        let n = from.read(&mut buf).await?;
        if n == 0 {
            break;
        }
        to.write_all(&buf[..n]).await?;
        to.flush().await?;
        count.fetch_add(n as u64, Ordering::Relaxed);
    }
    let _ = to.shutdown().await;
    Ok(())
}

/// Passes bytes both ways until the server side ends (the connection is over), or the game side ends and the server
/// has had `GRACE` to finish its answer. Returns the counts and how it ended.
async fn pipe<G: AsyncRead + AsyncWrite, S: AsyncRead + AsyncWrite>(game: G, server: S) -> String {
    let (gr, gw) = tokio::io::split(game);
    let (sr, sw) = tokio::io::split(server);
    let (up, down) = (AtomicU64::new(0), AtomicU64::new(0));
    let to_server = pump(gr, sw, &up);
    let to_game = pump(sr, gw, &down);
    tokio::pin!(to_server, to_game);
    let how = |r: std::io::Result<()>, who: &str| match r {
        Ok(()) => format!("{who} closed"),
        Err(e) => format!("{who} side ended: {e}"),
    };
    let end = tokio::select! {
        r = &mut to_game => how(r, "server"),
        r = &mut to_server => {
            let first = how(r, "game");
            match tokio::time::timeout(GRACE, &mut to_game).await {
                Ok(_) => first,
                Err(_) => format!("{first}, server kept open"),
            }
        }
    };
    format!("game->server {} B, server->game {} B, {end}", up.load(Ordering::Relaxed), down.load(Ordering::Relaxed))
}

#[cfg(test)]
mod tests {
    use super::short_id;

    #[test]
    fn short_ids() {
        assert_eq!(short_id("acct-abcd2345efgh"), "acct-abcd..");
        assert_eq!(short_id("dev-wxyz6789mnop"), "dev-wxyz..");
        assert_eq!(short_id("dev-test"), "dev-test");
        assert_eq!(short_id("dev-abc"), "dev-abc");
        // no kind in front: the first 4 characters of the whole id
        assert_eq!(short_id("0123456789abcdef"), "0123..");
        assert_eq!(short_id("Acct-abcd2345"), "Acct..");
        assert_eq!(short_id(""), "");
    }
}
