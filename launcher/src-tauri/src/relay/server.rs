// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay server: an accept thread, per client a reader thread (parses frames, applies them to the room table) and a
//! writer thread fed by a bounded queue. All room/client state sits in one mutex; nothing blocks while holding it
//! (sends are `try_send` into the queues).

use super::codec::{self, MAX_FRAME};
use super::proto::{err, left, ClientMsg, CreateReq, NodeInfo, RoomInfo, ServerMsg, MAX_NODES, PROTO};
use serde::Serialize;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::io::{self, ErrorKind, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream, ToSocketAddrs};
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
}

impl Default for Config {
    fn default() -> Self {
        Config {
            build_stamp: None,
            password: None,
            keepalive_timeout: Duration::from_secs(15),
            server_version: format!("pokopia-launcher {}", env!("CARGO_PKG_VERSION")),
            log: None,
        }
    }
}

/// Snapshot for the UI.
#[derive(Serialize, Clone, Debug)]
pub struct Status {
    pub addr: String,
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
    pub players: Vec<PlayerStatus>,
}

#[derive(Serialize, Clone, Debug)]
pub struct PlayerStatus {
    pub nickname: String,
    pub vip: String,
    pub node_id: u8,
    pub host: bool,
}

struct Client {
    /// Empty until HELLO.
    nickname: String,
    tx: SyncSender<Vec<u8>>,
    /// For a forced drop: shutting it down ends the reader and writer.
    sock: TcpStream,
    /// (room id, host octet N of 169.254.R.N).
    room: Option<(u32, u8)>,
}

struct Room {
    id: u32,
    local_comm_id: u64,
    scene_id: u16,
    comm_version: i16,
    node_count_max: u8,
    security_mode: u8,
    pass_hash: [u8; 16],
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

#[derive(Default)]
struct State {
    clients: HashMap<u32, Client>,
    rooms: BTreeMap<u32, Room>,
}

impl State {
    /// Queues a frame. A full queue drops the client (its reader then cleans up).
    fn send(&self, id: u32, frame: Vec<u8>) {
        if let Some(c) = self.clients.get(&id) {
            if let Err(TrySendError::Full(_)) = c.tx.try_send(frame) {
                let _ = c.sock.shutdown(Shutdown::Both);
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
        if n == 1 {
            let r = self.rooms.remove(&room)?;
            for (_, other) in r.members.iter().filter(|(&m, _)| m != 1) {
                if let Some(c) = self.clients.get_mut(other) {
                    c.room = None;
                }
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
}

impl RelayServer {
    pub fn start(bind: impl ToSocketAddrs, cfg: Config) -> io::Result<RelayServer> {
        let listener = TcpListener::bind(bind)?;
        listener.set_nonblocking(true)?;
        let addr = listener.local_addr()?;
        let shared = Arc::new(Shared {
            cfg,
            state: Mutex::new(State::default()),
            stop: AtomicBool::new(false),
            next_id: AtomicU32::new(1),
            threads: Mutex::new(Vec::new()),
        });
        shared.log(&format!("relay listening on {}", addr));
        let sh = shared.clone();
        let accept = std::thread::Builder::new().name("relay-accept".into()).spawn(move || accept_loop(sh, listener))?;
        Ok(RelayServer { shared, addr, accept: Some(accept) })
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
                players: st
                    .info(r)
                    .nodes
                    .into_iter()
                    .map(|n| PlayerStatus { nickname: n.nickname, vip: vip_str(n.vip), node_id: n.node_id, host: n.node_id == 0 })
                    .collect(),
            })
            .collect();
        let mut lobby: Vec<String> =
            st.clients.values().filter(|c| c.room.is_none() && !c.nickname.is_empty()).map(|c| c.nickname.clone()).collect();
        lobby.sort();
        Status { addr: self.addr.to_string(), clients: st.clients.len(), lobby, rooms }
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
    st.clients.insert(id, Client { nickname: String::new(), tx, sock: sock.try_clone()?, room: None });
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
        let nick = st.clients.remove(&id).map(|c| c.nickname).unwrap_or_default();
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
    if proto != PROTO {
        return refuse(err::BAD_PROTO, "This server speaks another protocol version. Update the game or the launcher.", "bad proto");
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
        ClientMsg::Scan { local_comm_id, scene_id } => {
            let rooms = st
                .rooms
                .values()
                .filter(|r| r.members.len() < r.node_count_max as usize)
                .filter(|r| local_comm_id == 0 || r.local_comm_id == local_comm_id)
                .filter(|r| scene_id == 0xffff || r.scene_id == scene_id)
                .map(|r| st.info(r))
                .collect();
            st.send(id, ServerMsg::RoomList(rooms).encode());
        }
        ClientMsg::Create(c) => {
            if my_room.is_some() {
                st.send(id, error(err::BAD_FRAME, "Already in a room."));
            } else if let Some(room) = create(&mut st, id, c) {
                drop(st);
                sh.log(&format!("room {} created by client {}", room, id));
            } else {
                st.send(id, error(err::ROOM_FULL, "The server has no free room."));
            }
        }
        ClientMsg::Join { room_id, comm_version, pass_hash } => {
            if my_room.is_some() {
                st.send(id, error(err::BAD_FRAME, "Already in a room."));
            } else if let Err((code, text)) = join(&mut st, id, room_id, comm_version, pass_hash) {
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
            let Some((room, n)) = my_room else { return Flow::Go };
            let Some(r) = st.rooms.get(&room) else { return Flow::Go };
            let frame = ServerMsg::Datagram { src_ip: vip(room, n), src_port, dst_ip, dst_port, data }.encode();
            if dst_ip == vip(room, 255) {
                for (_, other) in r.members.iter().filter(|(&m, _)| m != n) {
                    st.send(*other, frame.clone());
                }
            } else if dst_ip & !0xff == vip(room, 0) && (dst_ip & 0xff) as u8 != n {
                if let Some(other) = r.members.get(&((dst_ip & 0xff) as u8)) {
                    st.send(*other, frame);
                }
            }
        }
    }
    Flow::Go
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
            adv: c.adv,
            members: BTreeMap::from([(1, id)]),
            rejected: HashSet::new(),
        },
    );
    st.clients.get_mut(&id)?.room = Some((room, 1));
    st.send(id, ServerMsg::Joined { room_id: room, vip: vip(room, 1), node_id: 0 }.encode());
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
    if r.pass_hash != pass_hash {
        return Err((err::PASSWORD, "Wrong room passphrase."));
    }
    let n = (2..=MAX_NODES).find(|n| !r.members.contains_key(n)).ok_or((err::ROOM_FULL, "That room is full."))?;
    r.members.insert(n, id);
    if let Some(c) = st.clients.get_mut(&id) {
        c.room = Some((room, n));
    }
    st.send(id, ServerMsg::Joined { room_id: room, vip: vip(room, n), node_id: n - 1 }.encode());
    st.push_room_state(room);
    Ok(())
}
