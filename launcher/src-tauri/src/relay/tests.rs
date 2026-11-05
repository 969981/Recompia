// SPDX-License-Identifier: GPL-3.0-or-later
//! Codec round-trips and malformed input; a live server driven by raw TCP test clients.

use super::codec::{self, FrameError, MAX_FRAME};
use super::proto::*;
use super::server::{Config, RelayServer};
use std::io::{ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::time::{Duration, Instant};

fn create_req(max: u8) -> CreateReq {
    CreateReq {
        local_comm_id: 0x0100_1234_5678_0000,
        scene_id: 1,
        comm_version: 3,
        node_count_max: max,
        security_mode: 1,
        pass_hash: [7; 16],
        adv: b"adv-1".to_vec(),
    }
}

fn room_info(nodes: u8, adv: usize) -> RoomInfo {
    RoomInfo {
        room_id: 5,
        local_comm_id: 0xdead_beef_0102_0304,
        scene_id: 0xffff,
        comm_version: -2,
        node_count_max: 8,
        security_mode: 2,
        nodes: (0..nodes).map(|n| NodeInfo { node_id: n, vip: 0xa9fe_0501 + n as u32, mac: [2, 0, 0xa9, 0xfe, 5, n + 1], nickname: "x".repeat(32) }).collect(),
        adv: vec![0xab; adv],
    }
}

fn client_msgs() -> Vec<ClientMsg> {
    vec![
        ClientMsg::Hello { proto: 1, build: "stamp".into(), nickname: "ナンジャモ".into(), password: "".into() },
        ClientMsg::Scan { local_comm_id: u64::MAX, scene_id: 0xffff },
        ClientMsg::Create(create_req(8)),
        ClientMsg::Join { room_id: 0xffff_fffe, comm_version: -1, pass_hash: [0xee; 16] },
        ClientMsg::Leave,
        ClientMsg::Advertise(vec![1; MAX_ADV]),
        ClientMsg::Reject { vip: 0xa9fe_0104 },
        ClientMsg::Datagram { src_port: 49530, dst_ip: 0xa9fe_01ff, dst_port: 49530, data: vec![9; MAX_DATA] },
        ClientMsg::Ping(0x1234_5678),
    ]
}

fn server_msgs() -> Vec<ServerMsg> {
    vec![
        ServerMsg::Welcome { client_id: 7, version: "v".into() },
        ServerMsg::Error { code: err::RATE, message: "slow down".into() },
        ServerMsg::RoomList(vec![room_info(0, 0), room_info(8, MAX_ADV)]),
        ServerMsg::Joined { room_id: 1, vip: 0xa9fe_0102, node_id: 1 },
        ServerMsg::RoomState(room_info(3, 10)),
        ServerMsg::Datagram { src_ip: 1, src_port: 2, dst_ip: 3, dst_port: 4, data: vec![5; MAX_DATA] },
        ServerMsg::Left(left::SHUTDOWN),
        ServerMsg::Pong(42),
    ]
}

fn split_one(frame: &[u8]) -> (u8, &[u8]) {
    let (ty, payload, used) = codec::split(frame).unwrap().unwrap();
    assert_eq!(used, frame.len());
    (ty, payload)
}

#[test]
fn codec_round_trips() {
    for m in client_msgs() {
        let f = m.encode();
        assert!(f.len() - 4 <= MAX_FRAME);
        let (ty, p) = split_one(&f);
        assert_eq!(ClientMsg::decode(ty, p).unwrap(), m);
    }
    for m in server_msgs() {
        let f = m.encode();
        assert!(f.len() - 4 <= MAX_FRAME);
        let (ty, p) = split_one(&f);
        assert_eq!(ServerMsg::decode(ty, p).unwrap(), m);
    }
}

#[test]
fn codec_rejects_malformed() {
    // short header / short body: need more bytes
    assert_eq!(codec::split(&[1, 0]), Ok(None));
    assert_eq!(codec::split(&[5, 0, 0, 0, 0x09, 1]), Ok(None));
    // len 0, len over the cap (rejected from the header alone)
    assert_eq!(codec::split(&[0, 0, 0, 0]), Err(FrameError::Empty));
    assert_eq!(codec::split(&(MAX_FRAME as u32 + 1).to_le_bytes()), Err(FrameError::TooLong(MAX_FRAME as u32 + 1)));
    assert_eq!(codec::split(&u32::MAX.to_le_bytes()), Err(FrameError::TooLong(u32::MAX)));
    // unknown types, both directions
    for ty in [0x00, 0x0a, 0x80, 0x89, 0xff] {
        assert!(ClientMsg::decode(ty, &[0; 16]).is_err());
        assert!(ServerMsg::decode(ty, &[0; 16]).is_err());
    }
    assert!(ClientMsg::decode(0x81, &[0; 6]).is_err()); // a server type sent by a client
    // every truncation and one trailing byte of every message
    for m in client_msgs() {
        let f = m.encode();
        let (ty, p) = split_one(&f);
        for cut in 0..p.len() {
            assert!(ClientMsg::decode(ty, &p[..cut]).is_err(), "{:?} cut {}", m, cut);
        }
        let mut long = p.to_vec();
        long.push(0);
        assert!(ClientMsg::decode(ty, &long).is_err());
    }
    for m in server_msgs() {
        let f = m.encode();
        let (ty, p) = split_one(&f);
        for cut in 0..p.len() {
            assert!(ServerMsg::decode(ty, &p[..cut]).is_err());
        }
    }
    // field caps
    let enc = |m: ClientMsg| {
        let f = m.encode();
        let (ty, p) = split_one(&f);
        ClientMsg::decode(ty, p)
    };
    assert!(enc(ClientMsg::Create(create_req(0))).is_err());
    assert!(enc(ClientMsg::Create(create_req(9))).is_err());
    assert!(enc(ClientMsg::Advertise(vec![0; MAX_ADV + 1])).is_err());
    assert!(enc(ClientMsg::Datagram { src_port: 0, dst_ip: 0, dst_port: 0, data: vec![0; MAX_DATA + 1] }).is_err());
    assert!(enc(ClientMsg::Hello { proto: 1, build: "".into(), nickname: "n".repeat(MAX_NICK + 1), password: "".into() }).is_err());
    // invalid UTF-8 in a str
    assert!(ClientMsg::decode(0x01, &[1, 0, 1, 0xff, 0, 0]).is_err());
}

#[test]
fn room_list_is_cut_to_the_frame_cap() {
    let rooms: Vec<RoomInfo> = (0..64).map(|_| room_info(8, MAX_ADV)).collect();
    let f = ServerMsg::RoomList(rooms).encode();
    assert!(f.len() - 4 <= MAX_FRAME);
    let (ty, p) = split_one(&f);
    let ServerMsg::RoomList(got) = ServerMsg::decode(ty, p).unwrap() else { panic!() };
    assert!(got.len() >= 10 && got.len() < 64, "{}", got.len());
}

// ---- live server ----

const H: [u8; 16] = [7; 16];

fn ip(r: u8, n: u8) -> u32 {
    u32::from_be_bytes([169, 254, r, n])
}

struct Tc {
    s: TcpStream,
    buf: Vec<u8>,
}

impl Tc {
    fn connect(addr: SocketAddr) -> Tc {
        let s = TcpStream::connect(addr).unwrap();
        s.set_nodelay(true).unwrap();
        Tc { s, buf: Vec::new() }
    }
    fn send(&mut self, m: ClientMsg) {
        self.s.write_all(&m.encode()).unwrap();
    }
    /// Next message within `wait`; None on timeout. Panics on EOF.
    fn try_recv(&mut self, wait: Duration) -> Option<ServerMsg> {
        let end = Instant::now() + wait;
        loop {
            if let Some((ty, p, used)) = codec::split(&self.buf).unwrap() {
                let m = ServerMsg::decode(ty, p).unwrap();
                self.buf.drain(..used);
                return Some(m);
            }
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return None;
            }
            self.s.set_read_timeout(Some(left)).unwrap();
            let mut chunk = [0u8; 8192];
            match self.s.read(&mut chunk) {
                Ok(0) => panic!("connection closed"),
                Ok(n) => self.buf.extend_from_slice(&chunk[..n]),
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => return None,
                Err(e) => panic!("read: {}", e),
            }
        }
    }
    fn recv(&mut self) -> ServerMsg {
        self.try_recv(Duration::from_secs(5)).expect("no message within 5 s")
    }
    /// The server closed the connection (after any queued messages, which are skipped).
    fn closed(&mut self, wait: Duration) -> bool {
        self.s.set_read_timeout(Some(wait)).unwrap();
        let mut chunk = [0u8; 8192];
        loop {
            match self.s.read(&mut chunk) {
                Ok(0) => return true,
                Ok(_) => continue,
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => return false,
                Err(_) => return true, // reset
            }
        }
    }
    /// Barrier: PING/PONG must be the next message, so nothing else is queued for this client.
    fn quiet(&mut self) {
        self.send(ClientMsg::Ping(77));
        assert_eq!(self.recv(), ServerMsg::Pong(77));
    }
    fn expect_error(&mut self, code: u16) {
        match self.recv() {
            ServerMsg::Error { code: c, .. } => assert_eq!(c, code),
            m => panic!("expected ERROR {}, got {:?}", code, m),
        }
    }
    fn room_state(&mut self) -> RoomInfo {
        match self.recv() {
            ServerMsg::RoomState(r) => r,
            m => panic!("expected ROOM_STATE, got {:?}", m),
        }
    }
    fn rooms(&mut self, local_comm_id: u64, scene_id: u16) -> Vec<RoomInfo> {
        self.send(ClientMsg::Scan { local_comm_id, scene_id });
        match self.recv() {
            ServerMsg::RoomList(r) => r,
            m => panic!("expected ROOM_LIST, got {:?}", m),
        }
    }
}

fn hello_msg(build: &str, nick: &str, pw: &str) -> ClientMsg {
    ClientMsg::Hello { proto: PROTO, build: build.into(), nickname: nick.into(), password: pw.into() }
}

fn player(addr: SocketAddr, nick: &str) -> Tc {
    let mut c = Tc::connect(addr);
    c.send(hello_msg("B1", nick, "pw"));
    assert!(matches!(c.recv(), ServerMsg::Welcome { .. }));
    c
}

fn vips(r: &RoomInfo) -> Vec<(u8, u32, String)> {
    r.nodes.iter().map(|n| (n.node_id, n.vip, n.nickname.clone())).collect()
}

fn server(timeout: Duration) -> RelayServer {
    let cfg = Config { build_stamp: Some("B1".into()), password: Some("pw".into()), keepalive_timeout: timeout, ..Default::default() };
    RelayServer::start("127.0.0.1:0", cfg).unwrap()
}

#[test]
fn hello_is_checked() {
    let srv = server(Duration::from_secs(15));
    let addr = srv.local_addr();
    for (msg, code) in [
        (hello_msg("B2", "a", "pw"), err::BUILD),
        (hello_msg("B1", "a", "nope"), err::PASSWORD),
        (ClientMsg::Hello { proto: 2, build: "B1".into(), nickname: "a".into(), password: "pw".into() }, err::BAD_PROTO),
        (ClientMsg::Scan { local_comm_id: 0, scene_id: 0xffff }, err::BAD_FRAME),
    ] {
        let mut c = Tc::connect(addr);
        c.send(msg);
        c.expect_error(code);
        assert!(c.closed(Duration::from_secs(5)));
    }
    // malformed frames after a good hello: oversized header, unknown type
    for raw in [(MAX_FRAME as u32 + 1).to_le_bytes().to_vec(), vec![1, 0, 0, 0, 0x55]] {
        let mut c = player(addr, "m");
        c.s.write_all(&raw).unwrap();
        c.expect_error(err::BAD_FRAME);
        assert!(c.closed(Duration::from_secs(5)));
    }
}

#[test]
fn four_players() {
    let srv = server(Duration::from_secs(15));
    let addr = srv.local_addr();
    let mut host = player(addr, "host");
    let (mut g1, mut g2, mut g3) = (player(addr, "g1"), player(addr, "g2"), player(addr, "g3"));

    host.send(ClientMsg::Create(create_req(4)));
    assert_eq!(host.recv(), ServerMsg::Joined { room_id: 1, vip: ip(1, 1), node_id: 0 });
    let r = host.room_state();
    assert_eq!(vips(&r), vec![(0, ip(1, 1), "host".into())]);
    assert_eq!(r.nodes[0].mac, [2, 0, 0xa9, 0xfe, 1, 1]);
    assert_eq!(r.adv, b"adv-1");

    // scan filters
    assert_eq!(g1.rooms(0, 0xffff).len(), 1);
    assert_eq!(g1.rooms(0x0100_1234_5678_0000, 1)[0].room_id, 1);
    assert!(g1.rooms(0x0100_1234_5678_0001, 0xffff).is_empty());
    assert!(g1.rooms(0, 2).is_empty());

    // join errors
    g1.send(ClientMsg::Join { room_id: 9, comm_version: 3, pass_hash: H });
    g1.expect_error(err::NO_ROOM);
    g1.send(ClientMsg::Join { room_id: 1, comm_version: 4, pass_hash: H });
    g1.expect_error(err::VERSION);
    g1.send(ClientMsg::Join { room_id: 1, comm_version: 3, pass_hash: [8; 16] });
    g1.expect_error(err::PASSWORD);

    // three joins: .2 .3 .4, ROOM_STATE to every member
    let mut members = vec![(0, ip(1, 1), "host".to_string())];
    for (i, g) in [&mut g1, &mut g2, &mut g3].into_iter().enumerate() {
        let n = i as u8 + 2;
        g.send(ClientMsg::Join { room_id: 1, comm_version: 3, pass_hash: H });
        assert_eq!(g.recv(), ServerMsg::Joined { room_id: 1, vip: ip(1, n), node_id: n - 1 });
        members.push((n - 1, ip(1, n), format!("g{}", i + 1)));
        assert_eq!(vips(&g.room_state()), members);
    }
    // earlier members saw each join
    host.room_state();
    host.room_state();
    assert_eq!(vips(&host.room_state()), members);
    g1.room_state();
    assert_eq!(vips(&g1.room_state()), members);
    assert_eq!(vips(&g2.room_state()), members);
    for c in [&mut host, &mut g1, &mut g2, &mut g3] {
        c.quiet();
    }

    // room full: a 5th player cannot join and no longer sees it in SCAN
    let mut g4 = player(addr, "g4");
    assert!(g4.rooms(0, 0xffff).is_empty());
    g4.send(ClientMsg::Join { room_id: 1, comm_version: 3, pass_hash: H });
    g4.expect_error(err::ROOM_FULL);

    let st = srv.status();
    assert_eq!(st.clients, 5);
    assert_eq!(st.lobby, vec!["g4".to_string()]);
    assert_eq!(st.rooms[0].players.iter().map(|p| (p.nickname.as_str(), p.vip.as_str())).collect::<Vec<_>>(),
        vec![("host", "169.254.1.1"), ("g1", "169.254.1.2"), ("g2", "169.254.1.3"), ("g3", "169.254.1.4")]);

    // unicast g1 -> g2 (.3): src_ip rewritten to g1's vip, nobody else gets it; unknown vip dropped
    g1.send(ClientMsg::Datagram { src_port: 49530, dst_ip: ip(1, 3), dst_port: 49531, data: b"hi".to_vec() });
    g1.send(ClientMsg::Datagram { src_port: 49530, dst_ip: ip(1, 7), dst_port: 49530, data: b"lost".to_vec() });
    g1.send(ClientMsg::Datagram { src_port: 49530, dst_ip: ip(2, 3), dst_port: 49530, data: b"lost".to_vec() });
    g1.quiet();
    assert_eq!(g2.recv(), ServerMsg::Datagram { src_ip: ip(1, 2), src_port: 49530, dst_ip: ip(1, 3), dst_port: 49531, data: b"hi".to_vec() });
    for c in [&mut host, &mut g2, &mut g3] {
        c.quiet();
    }

    // broadcast from the host reaches the 3 guests, not the host
    host.send(ClientMsg::Datagram { src_port: 49530, dst_ip: ip(1, 255), dst_port: 49530, data: vec![0xbc; 4096] });
    host.quiet();
    for g in [&mut g1, &mut g2, &mut g3] {
        assert_eq!(g.recv(), ServerMsg::Datagram { src_ip: ip(1, 1), src_port: 49530, dst_ip: ip(1, 255), dst_port: 49530, data: vec![0xbc; 4096] });
        g.quiet();
    }
    // a player in no room cannot send into one
    g4.send(ClientMsg::Datagram { src_port: 1, dst_ip: ip(1, 255), dst_port: 1, data: vec![1] });
    g4.quiet();

    // host-only messages
    g1.send(ClientMsg::Advertise(b"evil".to_vec()));
    g1.expect_error(err::NOT_HOST);
    g1.send(ClientMsg::Reject { vip: ip(1, 3) });
    g1.expect_error(err::NOT_HOST);
    host.send(ClientMsg::Advertise(b"adv-2".to_vec()));
    for c in [&mut host, &mut g1, &mut g2, &mut g3] {
        assert_eq!(c.room_state().adv, b"adv-2");
    }

    // host rejects g3 (.4): LEFT 2, the rest see it gone, g3 cannot come back
    host.send(ClientMsg::Reject { vip: ip(1, 4) });
    assert_eq!(g3.recv(), ServerMsg::Left(left::REJECTED));
    members.pop();
    for c in [&mut host, &mut g1, &mut g2] {
        assert_eq!(vips(&c.room_state()), members);
    }
    g3.send(ClientMsg::Join { room_id: 1, comm_version: 3, pass_hash: H });
    g3.expect_error(err::REJECTED);

    // guest leaves: LEFT 0, ROOM_STATE to the rest
    g2.send(ClientMsg::Leave);
    assert_eq!(g2.recv(), ServerMsg::Left(left::YOU_LEFT));
    members.pop();
    for c in [&mut host, &mut g1] {
        assert_eq!(vips(&c.room_state()), members);
    }
    // g4 takes the lowest free address again (.3)
    g4.send(ClientMsg::Join { room_id: 1, comm_version: 3, pass_hash: H });
    assert_eq!(g4.recv(), ServerMsg::Joined { room_id: 1, vip: ip(1, 3), node_id: 2 });
    g4.room_state();
    host.room_state();
    g1.room_state();

    // host drops: LEFT 1 to everyone left, the room is gone from SCAN
    drop(host);
    assert_eq!(g1.recv(), ServerMsg::Left(left::HOST_CLOSED));
    assert_eq!(g4.recv(), ServerMsg::Left(left::HOST_CLOSED));
    assert!(g2.rooms(0, 0xffff).is_empty());
    assert!(srv.status().rooms.is_empty());
    // after the room closed the guests are in no room: a new CREATE gets room 1 again
    g1.send(ClientMsg::Create(create_req(2)));
    assert_eq!(g1.recv(), ServerMsg::Joined { room_id: 1, vip: ip(1, 1), node_id: 0 });
}

#[test]
fn keepalive_timeout_drops_silent_clients() {
    let srv = server(Duration::from_millis(400));
    let addr = srv.local_addr();
    let mut host = player(addr, "host");
    host.send(ClientMsg::Create(create_req(8)));
    host.recv();
    host.room_state();
    let mut guest = player(addr, "guest");
    guest.send(ClientMsg::Join { room_id: 1, comm_version: 3, pass_hash: H });
    guest.recv();
    guest.room_state();
    host.room_state();
    // the guest keeps pinging, the host goes silent
    let t = Instant::now();
    let mut closed = false;
    while t.elapsed() < Duration::from_secs(3) {
        guest.send(ClientMsg::Ping(1));
        match guest.recv() {
            ServerMsg::Pong(1) => {}
            ServerMsg::Left(reason) => {
                assert_eq!(reason, left::HOST_CLOSED);
                closed = true;
                break;
            }
            m => panic!("{:?}", m),
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    assert!(closed, "host was not timed out");
    assert!(t.elapsed() >= Duration::from_millis(300));
    assert!(host.closed(Duration::from_secs(2)));
    assert!(srv.status().rooms.is_empty());
    // a connection that never says HELLO times out too
    let mut silent = Tc::connect(addr);
    assert!(silent.closed(Duration::from_secs(3)));
}

#[test]
fn rate_limit_and_server_full_and_stop() {
    let mut srv = server(Duration::from_secs(15));
    let addr = srv.local_addr();
    // 2100 frames in one burst (datagrams outside a room: no replies): ERROR 9, then closed
    let mut flood = player(addr, "flood");
    let burst: Vec<u8> = (0..2100).flat_map(|_| ClientMsg::Datagram { src_port: 1, dst_ip: ip(1, 255), dst_port: 1, data: vec![0] }.encode()).collect();
    flood.s.write_all(&burst).unwrap();
    flood.expect_error(err::RATE);
    assert!(flood.closed(Duration::from_secs(5)));

    // 32 connections max: the 33rd gets ERROR 4 and is closed
    let deadline = Instant::now() + Duration::from_secs(5);
    while srv.status().clients != 0 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
    }
    let mut all: Vec<Tc> = (0..32).map(|i| player(addr, &format!("p{}", i))).collect();
    let mut extra = Tc::connect(addr);
    extra.expect_error(err::ROOM_FULL);
    assert!(extra.closed(Duration::from_secs(5)));

    // stop: LEFT 3 to room members, every connection closed
    all[0].send(ClientMsg::Create(create_req(8)));
    all[0].recv();
    all[0].room_state();
    srv.stop();
    assert_eq!(all[0].recv(), ServerMsg::Left(left::SHUTDOWN));
    for c in &mut all {
        assert!(c.closed(Duration::from_secs(5)));
    }
    assert!(TcpStream::connect_timeout(&addr, Duration::from_millis(500)).is_err());
}
