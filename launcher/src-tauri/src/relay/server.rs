// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay server: an accept thread, per client a reader thread (parses frames, applies them to the room table) and a
//! writer thread fed by a bounded queue. All room/client state sits in one mutex; nothing blocks while holding it
//! (sends are `try_send` into the queues, or one UDP `send_to`).
//! Protocol v3 adds a UDP socket on the same port and a thread reading it (`udp_loop`): datagrams of clients that proved
//! their UDP endpoint go over it, everyone else's over TCP as in v1.

use super::codec::{self, MAX_FRAME};
use super::proto::{err, left, udp, ClientMsg, CreateReq, NodeInfo, RoomInfo, ServerMsg, MAX_NODES, MIN_PROTO, NO_KEY, PROTO};
use serde::Serialize;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::io::{self, ErrorKind, Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

const MAX_ROOMS: usize = 64;
const MAX_CLIENTS: usize = 32;
const MAX_FRAMES_PER_SEC: u32 = 2000;
/// Frames queued for one client before it counts as too slow and is dropped.
const SEND_QUEUE: usize = 256;
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
/// UDP packets per second one token may send (the game sends about 50 a second per player); more are dropped.
const UDP_PACKETS_PER_SEC: u32 = 2000;
/// Wrong room codes one connection may present before it is dropped (ERROR 9).
const CODE_FAILS_PER_CONN: u32 = 5;
/// Wrong room codes (and first keyed scans that found nothing) from one IP address within CODE_FAIL_WINDOW; past it every request
/// carrying a room key from that address gets ERROR 9. A room code has 80 bits: this only stops an online guesser from trying
/// codes at line speed, it is not what keeps the code safe.
const CODE_FAILS_PER_IP: u32 = 30;
const CODE_FAIL_WINDOW: Duration = Duration::from_secs(600);

pub type LogFn = Arc<dyn Fn(&str) + Send + Sync>;

#[derive(Clone)]
pub struct Config {
    /// Game build stamp a HELLO must carry. None: any.
    pub build_stamp: Option<String>,
    /// Server password. None or empty: no password.
    pub password: Option<String>,
    /// A client silent this long is dropped (15 s in production; tests shorten it).
    pub keepalive_timeout: Duration,
    /// Sent in WELCOME.
    pub server_version: String,
    /// One plain line per event (start, connect/disconnect, room create/close).
    pub log: Option<LogFn>,
    /// Open the UDP datagram path (v3) on the same port. False, or the UDP bind failing: TCP only, as v2.
    pub udp: bool,
    /// UDP packets per second per token before the rest of that second is dropped.
    pub udp_rate: u32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            build_stamp: None,
            password: None,
            keepalive_timeout: Duration::from_secs(15),
            server_version: format!("pokopia-launcher {}", env!("CARGO_PKG_VERSION")),
            log: None,
            udp: true,
            udp_rate: UDP_PACKETS_PER_SEC,
        }
    }
}

/// Snapshot for the UI.
#[derive(Serialize, Clone, Debug)]
pub struct Status {
    pub addr: String,
    /// The UDP datagram path is open (same port as `addr`).
    pub udp: bool,
    /// Connections, including ones in no room.
    pub clients: usize,
    /// Nicknames of players connected but in no room.
    pub lobby: Vec<String>,
    pub rooms: Vec<RoomStatus>,
}

#[derive(Serialize, Clone, Debug)]
pub struct RoomStatus {
    pub room_id: u32,
    /// Hex: a u64 does not fit a JS number.
    pub local_comm_id: String,
    pub node_count_max: u8,
    /// Created with a room code: listed only to players presenting it (the code itself is never known to the server).
    pub private: bool,
    pub players: Vec<PlayerStatus>,
}

#[derive(Serialize, Clone, Debug)]
pub struct PlayerStatus {
    pub nickname: String,
    pub vip: String,
    pub node_id: u8,
    pub host: bool,
    /// Gets its datagrams over UDP (else TCP).
    pub udp: bool,
}

struct Client {
    /// Empty until HELLO.
    nickname: String,
    tx: SyncSender<Vec<u8>>,
    /// For a forced drop: shutting it down ends the reader and writer.
    sock: TcpStream,
    /// (room id, host octet N of 169.254.R.N).
    room: Option<(u32, u8)>,
    ip: IpAddr,
    /// The first room key this connection presented; another key later counts as a wrong code.
    key: Option<[u8; 16]>,
    /// Wrong room codes so far (CODE_FAILS_PER_CONN).
    code_fails: u32,
    /// A keyed SCAN that found no room was counted against the IP once already.
    miss_counted: bool,
    /// HELLO's proto (0 before HELLO).
    proto: u16,
    /// UDP token from UDP_OFFER, valid while in a room; 0: none (v2 client, no UDP socket, or in no room).
    token: u64,
    /// Where this client's last valid UDP HELLO came from.
    udp_addr: Option<SocketAddr>,
    /// The client said UDP_MODE 1: its datagrams go to udp_addr.
    udp_on: bool,
    /// Rate window start and UDP packets in it.
    udp_window: (Instant, u32),
}

struct Room {
    id: u32,
    local_comm_id: u64,
    scene_id: u16,
    comm_version: i16,
    node_count_max: u8,
    security_mode: u8,
    pass_hash: [u8; 16],
    /// None: an open room (listed to everyone, as in protocol 1).
    room_key: Option<[u8; 16]>,
    adv: Vec<u8>,
    /// Host octet N -> client id. N = 1 is the host.
    members: BTreeMap<u8, u32>,
    /// Client ids the host rejected.
    rejected: HashSet<u32>,
}

fn vip(room: u32, n: u8) -> u32 {
    0xa9fe_0000 | (room & 0xff) << 8 | n as u32
}

pub fn vip_str(v: u32) -> String {
    let b = v.to_be_bytes();
    format!("{}.{}.{}.{}", b[0], b[1], b[2], b[3])
}

/// Equal without an early exit (room keys and passphrase hashes).
fn ct_eq(a: &[u8; 16], b: &[u8; 16]) -> bool {
    let mut d = 0u8;
    for i in 0..16 {
        d |= a[i] ^ b[i];
    }
    std::hint::black_box(d) == 0
}

#[derive(Default)]
struct State {
    clients: HashMap<u32, Client>,
    rooms: BTreeMap<u32, Room>,
    /// Wrong room codes per address: (count, start of the window).
    ip_fails: HashMap<IpAddr, (u32, Instant)>,
    /// UDP token -> client id.
    tokens: HashMap<u64, u32>,
    /// The UDP socket (None: TCP only). Sends from under the state lock: UDP send_to does not wait on the peer.
    udp: Option<Arc<UdpSocket>>,
}

/// Outcome of a room-key check.
enum Code {
    Ok,
    /// ERROR 11, the connection stays open.
    Wrong,
    /// ERROR 9 + close (this connection or its address is over the budget).
    Limit,
}

impl State {
    fn ip_blocked(&mut self, ip: IpAddr) -> bool {
        match self.ip_fails.get(&ip) {
            Some(&(_, t)) if t.elapsed() >= CODE_FAIL_WINDOW => {
                self.ip_fails.remove(&ip);
                false
            }
            Some(&(n, _)) => n >= CODE_FAILS_PER_IP,
            None => false,
        }
    }

    fn count_ip_fail(&mut self, ip: IpAddr) {
        self.ip_fails.retain(|_, (_, t)| t.elapsed() < CODE_FAIL_WINDOW);
        self.ip_fails.entry(ip).or_insert((0, Instant::now())).0 += 1;
    }

    /// One wrong code: counted for the connection and its address.
    fn code_failed(&mut self, id: u32) -> Code {
        let Some(c) = self.clients.get_mut(&id) else { return Code::Limit };
        c.code_fails += 1;
        let (n, ip) = (c.code_fails, c.ip);
        self.count_ip_fail(ip);
        if n >= CODE_FAILS_PER_CONN || self.ip_blocked(ip) {
            Code::Limit
        } else {
            Code::Wrong
        }
    }

    /// A request carrying `key` (SCAN, CREATE, JOIN). NO_KEY always passes. A blocked address may present no key; a connection
    /// keeps the first key it presents, a different one later counts as a wrong code.
    fn present_key(&mut self, id: u32, key: &[u8; 16]) -> Code {
        if ct_eq(key, &NO_KEY) {
            return Code::Ok;
        }
        let Some(c) = self.clients.get(&id) else { return Code::Limit };
        let (ip, first) = (c.ip, c.key);
        if self.ip_blocked(ip) {
            return Code::Limit;
        }
        match first {
            None => {
                if let Some(c) = self.clients.get_mut(&id) {
                    c.key = Some(*key);
                }
                Code::Ok
            }
            Some(k) if ct_eq(&k, key) => Code::Ok,
            Some(_) => self.code_failed(id),
        }
    }

    /// Queues a frame. A full queue drops the client (its reader then cleans up).
    fn send(&self, id: u32, frame: Vec<u8>) {
        if let Some(c) = self.clients.get(&id) {
            if let Err(TrySendError::Full(_)) = c.tx.try_send(frame) {
                let _ = c.sock.shutdown(Shutdown::Both);
            }
        }
    }

    /// A v3 client entered a room (after an authorised CREATE/JOIN): a fresh random token for its UDP packets, sent as
    /// UDP_OFFER right behind JOINED. No UDP socket, a v2 client, or no random bytes: none, the client stays on TCP.
    fn udp_grant(&mut self, id: u32) {
        let Some(port) = self.udp.as_ref().and_then(|u| u.local_addr().ok()).map(|a| a.port()) else { return };
        if self.clients.get(&id).is_none_or(|c| c.proto < 3) {
            return;
        }
        let token = new_token();
        if token == 0 || self.tokens.contains_key(&token) {
            return;
        }
        self.udp_revoke(id);
        self.tokens.insert(token, id);
        if let Some(c) = self.clients.get_mut(&id) {
            c.token = token;
        }
        self.send(id, ServerMsg::UdpOffer { token, port }.encode());
    }

    /// The client left its room (or the connection): its token stops counting, its datagrams are TCP again.
    fn udp_revoke(&mut self, id: u32) {
        let Some(c) = self.clients.get_mut(&id) else { return };
        let token = std::mem::take(&mut c.token);
        c.udp_addr = None;
        c.udp_on = false;
        if token != 0 {
            self.tokens.remove(&token);
        }
    }

    /// One game datagram to client `id`: over UDP when it has a proven endpoint and asked for it, else a TCP frame.
    /// A failed UDP send is a lost datagram (UDP semantics), never a fallback.
    fn send_datagram(&self, id: u32, src_ip: u32, src_port: u16, dst_ip: u32, dst_port: u16, data: &[u8]) {
        let Some(c) = self.clients.get(&id) else { return };
        if let (true, Some(addr), Some(sock)) = (c.udp_on, c.udp_addr, &self.udp) {
            let pkt = udp::encode(c.token, &udp::Packet::SData { src_ip, src_port, dst_ip, dst_port, data });
            let _ = sock.send_to(&pkt, addr);
            return;
        }
        self.send(id, ServerMsg::Datagram { src_ip, src_port, dst_ip, dst_port, data: data.to_vec() }.encode());
    }

    /// A datagram from member `n` of `room` (either transport): src_ip is the sender's vip; .255 goes to every other member,
    /// a member's vip to that member; anything else is dropped.
    fn route(&self, room: u32, n: u8, src_port: u16, dst_ip: u32, dst_port: u16, data: &[u8]) {
        let Some(r) = self.rooms.get(&room) else { return };
        let src_ip = vip(room, n);
        if dst_ip == vip(room, 255) {
            for (_, other) in r.members.iter().filter(|(&m, _)| m != n) {
                self.send_datagram(*other, src_ip, src_port, dst_ip, dst_port, data);
            }
        } else if dst_ip & !0xff == vip(room, 0) && (dst_ip & 0xff) as u8 != n {
            if let Some(other) = r.members.get(&((dst_ip & 0xff) as u8)) {
                self.send_datagram(*other, src_ip, src_port, dst_ip, dst_port, data);
            }
        }
    }

    fn info(&self, r: &Room) -> RoomInfo {
        RoomInfo {
            room_id: r.id,
            local_comm_id: r.local_comm_id,
            scene_id: r.scene_id,
            comm_version: r.comm_version,
            node_count_max: r.node_count_max,
            security_mode: r.security_mode,
            nodes: r
                .members
                .iter()
                .map(|(&n, id)| NodeInfo {
                    node_id: n - 1,
                    vip: vip(r.id, n),
                    mac: [0x02, 0x00, 0xa9, 0xfe, r.id as u8, n],
                    nickname: self.clients.get(id).map(|c| c.nickname.clone()).unwrap_or_default(),
                })
                .collect(),
            adv: r.adv.clone(),
        }
    }

    fn push_room_state(&self, room: u32) {
        if let Some(r) = self.rooms.get(&room) {
            let frame = ServerMsg::RoomState(self.info(r)).encode();
            for id in r.members.values() {
                self.send(*id, frame.clone());
            }
        }
    }

    /// Takes `id` out of its room. `notify`: LEFT/ROOM_STATE to the others (false while the server stops).
    /// Returns a log line when a room closed.
    fn leave_room(&mut self, id: u32, notify: bool) -> Option<String> {
        let (room, n) = self.clients.get_mut(&id)?.room.take()?;
        self.udp_revoke(id);
        if n == 1 {
            let r = self.rooms.remove(&room)?;
            for (_, other) in r.members.iter().filter(|(&m, _)| m != 1) {
                if let Some(c) = self.clients.get_mut(other) {
                    c.room = None;
                }
                self.udp_revoke(*other);
                if notify {
                    self.send(*other, ServerMsg::Left(left::HOST_CLOSED).encode());
                }
            }
            return Some(format!("room {} closed", room));
        }
        if let Some(r) = self.rooms.get_mut(&room) {
            r.members.remove(&n);
        }
        if notify {
            self.push_room_state(room);
        }
        None
    }
}

struct Shared {
    cfg: Config,
    udp: Option<Arc<UdpSocket>>,
    state: Mutex<State>,
    stop: AtomicBool,
    next_id: AtomicU32,
    threads: Mutex<Vec<JoinHandle<()>>>,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
    fn log(&self, line: &str) {
        if let Some(f) = &self.cfg.log {
            f(line);
        }
    }
}

/// A running server. Dropping it stops it.
pub struct RelayServer {
    shared: Arc<Shared>,
    addr: SocketAddr,
    accept: Option<JoinHandle<()>>,
    udp_thread: Option<JoinHandle<()>>,
}

impl RelayServer {
    pub fn start(bind: impl ToSocketAddrs, cfg: Config) -> io::Result<RelayServer> {
        let listener = TcpListener::bind(bind)?;
        listener.set_nonblocking(true)?;
        let addr = listener.local_addr()?;
        // UDP on the same address and port. A failure (port taken by another program) only costs the UDP path.
        let mut udp_note = None;
        let udp = if cfg.udp {
            match UdpSocket::bind(addr).and_then(|u| u.set_read_timeout(Some(Duration::from_millis(100))).map(|_| u)) {
                Ok(u) => Some(Arc::new(u)),
                Err(e) => {
                    udp_note = Some(format!("UDP port {} unavailable ({}): datagrams go over TCP", addr.port(), e));
                    None
                }
            }
        } else {
            None
        };
        let shared = Arc::new(Shared {
            cfg,
            udp: udp.clone(),
            state: Mutex::new(State { udp, ..Default::default() }),
            stop: AtomicBool::new(false),
            next_id: AtomicU32::new(1),
            threads: Mutex::new(Vec::new()),
        });
        shared.log(&format!("relay listening on {} (TCP{})", addr, if shared.udp.is_some() { " and UDP" } else { "" }));
        if let Some(n) = udp_note {
            shared.log(&n);
        }
        let udp_thread = match shared.udp.clone() {
            Some(u) => {
                let sh = shared.clone();
                Some(std::thread::Builder::new().name("relay-udp".into()).spawn(move || udp_loop(sh, u))?)
            }
            None => None,
        };
        let sh = shared.clone();
        let accept = match std::thread::Builder::new().name("relay-accept".into()).spawn(move || accept_loop(sh, listener)) {
            Ok(a) => a,
            Err(e) => {
                shared.stop.store(true, Ordering::SeqCst);
                if let Some(t) = udp_thread {
                    let _ = t.join();
                }
                return Err(e);
            }
        };
        Ok(RelayServer { shared, addr, accept: Some(accept), udp_thread })
    }

    pub fn local_addr(&self) -> SocketAddr {
        self.addr
    }

    /// LEFT 3 to every room member, closes every connection, joins all threads.
    pub fn stop(&mut self) {
        let Some(accept) = self.accept.take() else { return };
        {
            let st = self.shared.lock();
            for (id, c) in &st.clients {
                if c.room.is_some() {
                    st.send(*id, ServerMsg::Left(left::SHUTDOWN).encode());
                }
            }
            self.shared.stop.store(true, Ordering::SeqCst);
        }
        let _ = accept.join();
        if let Some(t) = self.udp_thread.take() {
            let _ = t.join();
        }
        let threads = std::mem::take(&mut *self.shared.threads.lock().unwrap_or_else(|e| e.into_inner()));
        for t in threads {
            let _ = t.join();
        }
        self.shared.log("relay stopped");
    }

    pub fn status(&self) -> Status {
        let st = self.shared.lock();
        let rooms = st
            .rooms
            .values()
            .map(|r| RoomStatus {
                room_id: r.id,
                local_comm_id: format!("{:016x}", r.local_comm_id),
                node_count_max: r.node_count_max,
                private: r.room_key.is_some(),
                players: st
                    .info(r)
                    .nodes
                    .into_iter()
                    .map(|n| {
                        let udp = r.members.get(&(n.node_id + 1)).and_then(|id| st.clients.get(id)).is_some_and(|c| c.udp_on);
                        PlayerStatus { nickname: n.nickname, vip: vip_str(n.vip), node_id: n.node_id, host: n.node_id == 0, udp }
                    })
                    .collect(),
            })
            .collect();
        let mut lobby: Vec<String> =
            st.clients.values().filter(|c| c.room.is_none() && !c.nickname.is_empty()).map(|c| c.nickname.clone()).collect();
        lobby.sort();
        Status { addr: self.addr.to_string(), udp: self.shared.udp.is_some(), clients: st.clients.len(), lobby, rooms }
    }
}

impl Drop for RelayServer {
    fn drop(&mut self) {
        self.stop();
    }
}

fn accept_loop(sh: Arc<Shared>, listener: TcpListener) {
    while !sh.stop.load(Ordering::SeqCst) {
        let (sock, peer) = match listener.accept() {
            Ok(x) => x,
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::Interrupted => {
                std::thread::sleep(Duration::from_millis(20));
                continue;
            }
            Err(_) => {
                std::thread::sleep(Duration::from_millis(100));
                continue;
            }
        };
        if let Err(e) = admit(&sh, sock, peer) {
            sh.log(&format!("connection from {} failed: {}", peer, e));
        }
        // forget threads that already ended
        let mut threads = sh.threads.lock().unwrap_or_else(|e| e.into_inner());
        threads.retain(|t| !t.is_finished());
    }
}

fn admit(sh: &Arc<Shared>, mut sock: TcpStream, peer: SocketAddr) -> io::Result<()> {
    sock.set_nonblocking(false)?;
    sock.set_nodelay(true)?;
    sock.set_write_timeout(Some(WRITE_TIMEOUT))?;
    let mut st = sh.lock();
    if st.clients.len() >= MAX_CLIENTS {
        drop(st);
        sh.log(&format!("refused {}: server full", peer));
        let _ = sock.write_all(&ServerMsg::Error { code: err::ROOM_FULL, message: "The server is full.".into() }.encode());
        let _ = sock.shutdown(Shutdown::Write);
        return Ok(());
    }
    let id = sh.next_id.fetch_add(1, Ordering::SeqCst);
    let (tx, rx) = sync_channel(SEND_QUEUE);
    let writer_sock = sock.try_clone()?;
    let client = Client {
        nickname: String::new(),
        tx,
        sock: sock.try_clone()?,
        room: None,
        ip: peer.ip(),
        key: None,
        code_fails: 0,
        miss_counted: false,
        proto: 0,
        token: 0,
        udp_addr: None,
        udp_on: false,
        udp_window: (Instant::now(), 0),
    };
    st.clients.insert(id, client);
    drop(st);
    sh.log(&format!("client {} connected from {}", id, peer));
    let writer = std::thread::Builder::new().name(format!("relay-w{}", id)).spawn(move || writer_loop(writer_sock, rx));
    let sh2 = sh.clone();
    let reader = writer.and_then(|w| std::thread::Builder::new().name(format!("relay-r{}", id)).spawn(move || reader_loop(sh2, id, sock, w)));
    match reader {
        Ok(t) => {
            sh.threads.lock().unwrap_or_else(|e| e.into_inner()).push(t);
            Ok(())
        }
        Err(e) => {
            sh.lock().clients.remove(&id);
            Err(e)
        }
    }
}

/// Writes queued frames until every sender is gone (client removed) or a write fails (then the socket is shut so the
/// reader ends too).
fn writer_loop(mut sock: TcpStream, rx: Receiver<Vec<u8>>) {
    for frame in rx {
        if sock.write_all(&frame).is_err() {
            let _ = sock.shutdown(Shutdown::Both);
            return;
        }
    }
    let _ = sock.flush();
    let _ = sock.shutdown(Shutdown::Write);
}

enum Flow {
    Go,
    /// Close after the queued frames are written; the text goes to the log.
    Close(String),
}

fn reader_loop(sh: Arc<Shared>, id: u32, mut sock: TcpStream, writer: JoinHandle<()>) {
    let timeout = sh.cfg.keepalive_timeout;
    let poll = (timeout / 4).clamp(Duration::from_millis(10), Duration::from_millis(250));
    let _ = sock.set_read_timeout(Some(poll));
    let mut buf: Vec<u8> = Vec::with_capacity(2 * MAX_FRAME);
    let mut chunk = vec![0u8; 16 * 1024];
    let mut last_frame = Instant::now();
    let (mut window, mut frames_in_window) = (Instant::now(), 0u32);
    let mut hello_done = false;

    let why = 'outer: loop {
        if sh.stop.load(Ordering::SeqCst) {
            break "server stopped".to_string();
        }
        // every complete frame in the buffer
        loop {
            let (ty, msg, used) = match codec::split(&buf) {
                Ok(None) => break,
                Ok(Some((ty, payload, used))) => (ty, ClientMsg::decode(ty, payload), used),
                Err(_) => {
                    sh.lock().send(id, bad_frame());
                    break 'outer "bad frame header".to_string();
                }
            };
            buf.drain(..used);
            last_frame = Instant::now();
            if window.elapsed() >= Duration::from_secs(1) {
                (window, frames_in_window) = (Instant::now(), 0);
            }
            frames_in_window += 1;
            if frames_in_window > MAX_FRAMES_PER_SEC {
                sh.lock().send(id, error(err::RATE, "Too many messages; disconnected."));
                break 'outer "rate limit".to_string();
            }
            let Ok(msg) = msg else {
                sh.lock().send(id, bad_frame());
                break 'outer format!("bad frame (type 0x{:02x})", ty);
            };
            let flow = if hello_done { handle(&sh, id, msg) } else { hello(&sh, id, msg, &mut hello_done) };
            if let Flow::Close(why) = flow {
                break 'outer why;
            }
        }
        if last_frame.elapsed() >= timeout {
            break "keepalive timeout".to_string();
        }
        match sock.read(&mut chunk) {
            Ok(0) => break "closed".to_string(),
            Ok(n) => buf.extend_from_slice(&chunk[..n]),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => {}
            Err(_) => break "connection lost".to_string(),
        }
    };

    let (nick, closed) = {
        let mut st = sh.lock();
        let closed = st.leave_room(id, !sh.stop.load(Ordering::SeqCst));
        let gone = st.clients.remove(&id);
        if let Some(c) = gone.as_ref().filter(|c| c.token != 0) {
            st.tokens.remove(&c.token);
        }
        let nick = gone.map(|c| c.nickname).unwrap_or_default();
        (nick, closed)
    };
    if let Some(line) = closed {
        sh.log(&line);
    }
    sh.log(&format!("client {} ({}) disconnected: {}", id, if nick.is_empty() { "no hello" } else { &nick }, why));
    // the client entry (the queue's only sender) is gone: the writer drains what is queued, sends FIN and ends
    let _ = writer.join();
    // lingering close: read off what the peer still sends (briefly), else closing with unread input sends a RST
    // that can destroy the final ERROR before the peer reads it
    let _ = sock.set_read_timeout(Some(Duration::from_millis(50)));
    let end = Instant::now() + Duration::from_millis(500);
    while Instant::now() < end {
        match sock.read(&mut chunk) {
            Ok(0) => break,
            Ok(_) => {}
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => {}
            Err(_) => break,
        }
    }
}

fn error(code: u16, message: &str) -> Vec<u8> {
    ServerMsg::Error { code, message: message.into() }.encode()
}

fn bad_frame() -> Vec<u8> {
    error(err::BAD_FRAME, "Malformed message.")
}

/// Nickname for log lines and the UI: no control characters.
fn clean(s: &str) -> String {
    s.chars().filter(|c| !c.is_control()).collect()
}

fn hello(sh: &Shared, id: u32, msg: ClientMsg, done: &mut bool) -> Flow {
    let ClientMsg::Hello { proto, build, nickname, password } = msg else {
        sh.lock().send(id, error(err::BAD_FRAME, "HELLO must come first."));
        return Flow::Close("no hello".into());
    };
    let refuse = |code, text: &str, why: &str| {
        sh.lock().send(id, error(code, text));
        Flow::Close(why.into())
    };
    if proto < MIN_PROTO {
        let why = format!("bad proto {}", proto);
        return refuse(err::BAD_PROTO, "This server needs a newer game (relay protocol 2, room codes). Update the game.", &why);
    }
    if proto > PROTO {
        let why = format!("bad proto {}", proto);
        return refuse(err::BAD_PROTO, "This server is older than the game (relay protocol 3). Update the launcher that runs it.", &why);
    }
    if sh.cfg.build_stamp.as_deref().is_some_and(|s| s != build) {
        return refuse(err::BUILD, "This server runs another game build. Only identical builds can play together.", "build mismatch");
    }
    if sh.cfg.password.as_deref().is_some_and(|p| !p.is_empty() && p != password) {
        return refuse(err::PASSWORD, "Wrong server password.", "wrong password");
    }
    let nickname = clean(&nickname);
    let mut st = sh.lock();
    if let Some(c) = st.clients.get_mut(&id) {
        c.nickname = if nickname.is_empty() { format!("player{}", id) } else { nickname.clone() };
        c.proto = proto;
    }
    st.send(id, ServerMsg::Welcome { client_id: id, version: sh.cfg.server_version.clone() }.encode());
    drop(st);
    *done = true;
    sh.log(&format!("client {} is {}", id, nickname));
    Flow::Go
}

fn handle(sh: &Shared, id: u32, msg: ClientMsg) -> Flow {
    let mut st = sh.lock();
    let my_room = st.clients.get(&id).and_then(|c| c.room);
    match msg {
        ClientMsg::Hello { .. } => {
            st.send(id, error(err::BAD_FRAME, "HELLO was already sent."));
            return Flow::Close("second hello".into());
        }
        ClientMsg::Ping(n) => st.send(id, ServerMsg::Pong(n).encode()),
        ClientMsg::UdpMode(on) => {
            let Some(c) = st.clients.get_mut(&id) else { return Flow::Go };
            if c.proto < 3 {
                st.send(id, bad_frame());
                return Flow::Close("UDP_MODE from a v2 client".into());
            }
            // on: only with a proven endpoint (the client sends it after a UDP ACK, so it has one)
            let now_on = on && c.udp_addr.is_some();
            let changed = now_on != c.udp_on;
            c.udp_on = now_on;
            drop(st);
            if changed {
                sh.log(&format!("client {} datagrams over {}", id, if now_on { "UDP" } else { "TCP" }));
            }
        }
        ClientMsg::Scan { local_comm_id, scene_id, room_key } => {
            if let Some(flow) = key_refused(&mut st, id, &room_key) {
                return flow;
            }
            // open rooms, and the rooms whose key is the one presented (hidden from everyone else)
            let mut keyed_match = false;
            let rooms: Vec<RoomInfo> = st
                .rooms
                .values()
                .filter(|r| match &r.room_key {
                    None => true,
                    Some(k) => {
                        let hit = ct_eq(k, &room_key);
                        keyed_match |= hit;
                        hit
                    }
                })
                .filter(|r| r.members.len() < r.node_count_max as usize)
                .filter(|r| local_comm_id == 0 || r.local_comm_id == local_comm_id)
                .filter(|r| scene_id == 0xffff || r.scene_id == scene_id)
                .map(|r| st.info(r))
                .collect();
            // a guesser on fresh connections tries one key each: the first fruitless keyed scan of a connection counts for its address
            if !keyed_match && !ct_eq(&room_key, &NO_KEY) {
                if let Some(c) = st.clients.get_mut(&id).filter(|c| !c.miss_counted) {
                    c.miss_counted = true;
                    let ip = c.ip;
                    st.count_ip_fail(ip);
                }
            }
            st.send(id, ServerMsg::RoomList(rooms).encode());
        }
        ClientMsg::Create(c) => {
            if let Some(flow) = key_refused(&mut st, id, &c.room_key) {
                return flow;
            }
            let private = !ct_eq(&c.room_key, &NO_KEY);
            if my_room.is_some() {
                st.send(id, error(err::BAD_FRAME, "Already in a room."));
            } else if let Some(room) = create(&mut st, id, c) {
                drop(st);
                sh.log(&format!("room {} created by client {}{}", room, id, if private { " (room code)" } else { "" }));
            } else {
                st.send(id, error(err::ROOM_FULL, "The server has no free room."));
            }
        }
        ClientMsg::Join { room_id, comm_version, pass_hash, room_key } => {
            if let Some(flow) = key_refused(&mut st, id, &room_key) {
                return flow;
            }
            if my_room.is_some() {
                st.send(id, error(err::BAD_FRAME, "Already in a room."));
                return Flow::Go;
            }
            // a room with a code: the code is checked before anything else about the room is told
            let wrong = st.rooms.get(&room_id).and_then(|r| r.room_key).is_some_and(|k| !ct_eq(&k, &room_key));
            if wrong {
                return match st.code_failed(id) {
                    Code::Limit => limit(&st, id),
                    _ => {
                        st.send(id, error(err::ROOM_CODE, "Wrong room code."));
                        Flow::Go
                    }
                };
            }
            if let Err((code, text)) = join(&mut st, id, room_id, comm_version, pass_hash) {
                st.send(id, error(code, text));
            }
        }
        ClientMsg::Leave => {
            if my_room.is_some() {
                let closed = st.leave_room(id, true);
                st.send(id, ServerMsg::Left(left::YOU_LEFT).encode());
                drop(st);
                if let Some(line) = closed {
                    sh.log(&line);
                }
            }
        }
        ClientMsg::Advertise(adv) => match my_room {
            Some((room, 1)) => {
                if let Some(r) = st.rooms.get_mut(&room) {
                    r.adv = adv;
                }
                st.push_room_state(room);
            }
            _ => st.send(id, error(err::NOT_HOST, "Only the host can do that.")),
        },
        ClientMsg::Reject { vip: target } => match my_room {
            Some((room, 1)) => {
                let n = (target & 0xff) as u8;
                let member = st.rooms.get(&room).filter(|_| target & !0xff == vip(room, 0) && n != 1).and_then(|r| r.members.get(&n).copied());
                if let Some(other) = member {
                    st.leave_room(other, true);
                    if let Some(r) = st.rooms.get_mut(&room) {
                        r.rejected.insert(other);
                    }
                    st.send(other, ServerMsg::Left(left::REJECTED).encode());
                }
            }
            _ => st.send(id, error(err::NOT_HOST, "Only the host can do that.")),
        },
        ClientMsg::Datagram { src_port, dst_ip, dst_port, data } => {
            if let Some((room, n)) = my_room {
                st.route(room, n, src_port, dst_ip, dst_port, &data);
            }
        }
    }
    Flow::Go
}

fn limit(st: &State, id: u32) -> Flow {
    st.send(id, error(err::RATE, "Too many wrong room codes. Try again in a few minutes."));
    Flow::Close("too many wrong room codes".into())
}

/// Some(flow) when the key a request carries is refused (the ERROR is queued already).
fn key_refused(st: &mut State, id: u32, key: &[u8; 16]) -> Option<Flow> {
    match st.present_key(id, key) {
        Code::Ok => None,
        Code::Limit => Some(limit(st, id)),
        Code::Wrong => {
            st.send(id, error(err::ROOM_CODE, "Wrong room code."));
            Some(Flow::Go)
        }
    }
}

/// New room with `id` as host (.1, node 0). None: no free room id or the room cap is reached.
fn create(st: &mut State, id: u32, c: CreateReq) -> Option<u32> {
    if st.rooms.len() >= MAX_ROOMS {
        return None;
    }
    let room = (1..=254u32).find(|r| !st.rooms.contains_key(r))?;
    st.rooms.insert(
        room,
        Room {
            id: room,
            local_comm_id: c.local_comm_id,
            scene_id: c.scene_id,
            comm_version: c.comm_version,
            node_count_max: c.node_count_max,
            security_mode: c.security_mode,
            pass_hash: c.pass_hash,
            room_key: Some(c.room_key).filter(|k| !ct_eq(k, &NO_KEY)),
            adv: c.adv,
            members: BTreeMap::from([(1, id)]),
            rejected: HashSet::new(),
        },
    );
    st.clients.get_mut(&id)?.room = Some((room, 1));
    st.send(id, ServerMsg::Joined { room_id: room, vip: vip(room, 1), node_id: 0 }.encode());
    st.udp_grant(id);
    st.push_room_state(room);
    Some(room)
}

fn join(st: &mut State, id: u32, room: u32, comm_version: i16, pass_hash: [u8; 16]) -> Result<(), (u16, &'static str)> {
    let r = st.rooms.get_mut(&room).ok_or((err::NO_ROOM, "That room does not exist any more."))?;
    if r.rejected.contains(&id) {
        return Err((err::REJECTED, "The host turned you away from this room."));
    }
    if r.members.len() >= r.node_count_max as usize {
        return Err((err::ROOM_FULL, "That room is full."));
    }
    if r.comm_version != comm_version {
        return Err((err::VERSION, "That room runs another game version."));
    }
    if !ct_eq(&r.pass_hash, &pass_hash) {
        return Err((err::PASSWORD, "Wrong room passphrase."));
    }
    let n = (2..=MAX_NODES).find(|n| !r.members.contains_key(n)).ok_or((err::ROOM_FULL, "That room is full."))?;
    r.members.insert(n, id);
    if let Some(c) = st.clients.get_mut(&id) {
        c.room = Some((room, n));
    }
    st.send(id, ServerMsg::Joined { room_id: room, vip: vip(room, n), node_id: n - 1 }.encode());
    st.udp_grant(id);
    st.push_room_state(room);
    Ok(())
}

/// Reads the UDP socket until the server stops. Every packet is checked before anything is sent: a packet that is
/// malformed, carries an unknown token, comes from another IP than its token's TCP connection, or is over its token's rate
/// is dropped without an answer. The only answer is ACK to a valid HELLO, the same size as the HELLO, so the server never
/// sends an address more bytes than it got from it, and sends nothing to an address that did not prove a token.
fn udp_loop(sh: Arc<Shared>, sock: Arc<UdpSocket>) {
    let mut buf = vec![0u8; udp::MAX_PACKET + 1];
    while !sh.stop.load(Ordering::SeqCst) {
        // errors are timeouts, or on Windows ICMP port-unreachable resets from earlier sends: keep reading
        if let Ok((n, from)) = sock.recv_from(&mut buf) {
            udp_packet(&sh, &sock, &buf[..n], from);
        }
    }
}

fn udp_packet(sh: &Shared, sock: &UdpSocket, b: &[u8], from: SocketAddr) {
    let Some((token, pkt)) = udp::decode(b) else { return };
    let mut st = sh.lock();
    let Some(&id) = st.tokens.get(&token) else { return };
    let rate = sh.cfg.udp_rate;
    let Some(c) = st.clients.get_mut(&id) else { return };
    if from.ip() != c.ip {
        return;
    }
    let (start, count) = &mut c.udp_window;
    if start.elapsed() >= Duration::from_secs(1) {
        (*start, *count) = (Instant::now(), 0);
    }
    *count += 1;
    if *count > rate {
        return;
    }
    match pkt {
        udp::Packet::Hello => {
            // the endpoint is wherever the latest HELLO came from (a NAT may move it)
            let first = c.udp_addr.is_none();
            let moved = c.udp_addr.is_some_and(|a| a != from);
            c.udp_addr = Some(from);
            let _ = sock.send_to(&udp::encode(token, &udp::Packet::Ack), from);
            drop(st);
            if first || moved {
                sh.log(&format!("client {} UDP endpoint {}{}", id, from, if moved { " (moved)" } else { "" }));
            }
        }
        udp::Packet::Data { src_port, dst_ip, dst_port, data } => {
            if c.udp_addr != Some(from) {
                return;
            }
            if let Some((room, n)) = c.room {
                st.route(room, n, src_port, dst_ip, dst_port, data);
            }
        }
        udp::Packet::Ack | udp::Packet::SData { .. } => {}
    }
}

/// A random u64 token from the OS random source; 0 when that fails (the client then stays on TCP).
fn new_token() -> u64 {
    let mut b = [0u8; 8];
    if os_random(&mut b).is_err() {
        return 0;
    }
    u64::from_le_bytes(b)
}

#[cfg(windows)]
fn os_random(buf: &mut [u8]) -> io::Result<()> {
    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 2;
    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(alg: *mut std::ffi::c_void, buf: *mut u8, len: u32, flags: u32) -> i32;
    }
    let st = unsafe { BCryptGenRandom(std::ptr::null_mut(), buf.as_mut_ptr(), buf.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) };
    if st < 0 {
        return Err(io::Error::other(format!("BCryptGenRandom failed: 0x{:08x}", st as u32)));
    }
    Ok(())
}

#[cfg(not(windows))]
fn os_random(buf: &mut [u8]) -> io::Result<()> {
    std::fs::File::open("/dev/urandom")?.read_exact(buf)
}
