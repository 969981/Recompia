// SPDX-License-Identifier: GPL-3.0-or-later
//! pokopia-recomp extension: invite codes, the launcher's side (lib.rs "Invite codes"): keep an invite on a relay for its short code, and
//! look one up. One short blocking link each, every step within `timeout`.

use crate::engine::{E_LIMIT, E_NOROOM, HELLO_LEN, MAGIC, MSG_LEN, T_ERR, X_INVITE_DEL, X_INVITE_GET, X_INVITE_PUT};
use crate::invites::{self, Code, Token};
use std::io::{Read, Write};
use std::net::{SocketAddr, SocketAddrV4, TcpStream};
use std::time::Duration;

#[derive(Debug, PartialEq, Eq)]
pub enum Error {
    /// No invite is kept under that code (never was, or it expired).
    NotFound,
    /// Over this address's rate, the address keeps the relay's most invites already, or the relay could not draw a code.
    Limit,
    /// The relay does not keep invites (an older relay, or one run with --no-invites), or it refused the request as malformed.
    Unsupported,
    /// No answer: the relay is down, unreachable or too slow.
    Unreachable,
}

/// `IPv4[:port]` (the relay addresses the launcher keeps), the port defaulting to the relay's.
pub fn addr(s: &str) -> Option<SocketAddr> {
    let s = s.trim();
    s.parse::<SocketAddrV4>().ok().or_else(|| s.parse().ok().map(|ip| SocketAddrV4::new(ip, crate::DEFAULT_PORT))).map(SocketAddr::V4)
}

fn hello(kind: u8, room: &[u8], len: u32) -> [u8; HELLO_LEN] {
    let mut h = [0u8; HELLO_LEN];
    h[..4].copy_from_slice(&MAGIC.to_le_bytes());
    h[4] = kind;
    h[5] = 1;
    h[8..8 + room.len()].copy_from_slice(room);
    h[24..28].copy_from_slice(&len.to_le_bytes());
    h
}

fn send(relay: SocketAddr, parts: &[&[u8]], timeout: Duration) -> Result<TcpStream, Error> {
    let mut s = TcpStream::connect_timeout(&relay, timeout).map_err(|_| Error::Unreachable)?;
    s.set_read_timeout(Some(timeout)).and_then(|_| s.set_write_timeout(Some(timeout))).map_err(|_| Error::Unreachable)?;
    for p in parts {
        s.write_all(p).map_err(|_| Error::Unreachable)?;
    }
    Ok(s)
}

/// The relay's 16-byte answer: Ok when it is of type `want`, else what its error says.
fn answer(s: &mut TcpStream, want: u8) -> Result<[u8; MSG_LEN], Error> {
    let mut m = [0u8; MSG_LEN];
    s.read_exact(&mut m).map_err(|_| Error::Unreachable)?;
    if m[..4] != MAGIC.to_le_bytes() {
        return Err(Error::Unsupported);
    }
    match (m[4], m[5]) {
        (t, _) if t == want => Ok(m),
        (T_ERR, E_NOROOM) => Err(Error::NotFound),
        (T_ERR, E_LIMIT) => Err(Error::Limit),
        _ => Err(Error::Unsupported), // E_BAD: an older relay, or one without invites
    }
}

/// Keeps `invite` of the save named by `token` (invites::Token) on the relay for a few hours (lib.rs "Invite codes"; storing it, or a newer
/// invite of the same save, again restarts them): its code. A relay from before tokens ignores the token and keys on the invite alone.
pub fn store(relay: SocketAddr, invite: &[u8], token: &Token, timeout: Duration) -> Result<Code, Error> {
    if invite.is_empty() || invite.len() > invites::BLOB_MAX {
        return Err(Error::Unsupported);
    }
    let mut s = send(relay, &[&hello(X_INVITE_PUT, token, invite.len() as u32), invite], timeout)?;
    let m = answer(&mut s, X_INVITE_PUT)?;
    let code = &m[8..8 + invites::CODE_LEN];
    invites::normalize(&String::from_utf8_lossy(code)).ok_or(Error::Unsupported)
}

/// Drops the code of `invite` and of every invite of save `token` from the relay, so none of them opens the room. True: one was kept.
/// Unsupported: a relay from before deletes.
pub fn delete(relay: SocketAddr, invite: &[u8], token: &Token, timeout: Duration) -> Result<bool, Error> {
    if invite.is_empty() || invite.len() > invites::BLOB_MAX {
        return Err(Error::Unsupported);
    }
    let mut s = send(relay, &[&hello(X_INVITE_DEL, token, invite.len() as u32), invite], timeout)?;
    let m = answer(&mut s, X_INVITE_DEL)?;
    Ok(m[8] != 0)
}

/// The invite kept under `code`.
pub fn lookup(relay: SocketAddr, code: &Code, timeout: Duration) -> Result<Vec<u8>, Error> {
    let mut s = send(relay, &[&hello(X_INVITE_GET, code, 0)], timeout)?;
    let m = answer(&mut s, X_INVITE_GET)?;
    let n = u32::from_le_bytes([m[8], m[9], m[10], m[11]]) as usize;
    if n == 0 || n > invites::BLOB_MAX {
        return Err(Error::Unsupported);
    }
    let mut b = vec![0u8; n];
    s.read_exact(&mut b).map_err(|_| Error::Unreachable)?;
    Ok(b)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Config, RelayServer};

    const T: Duration = Duration::from_secs(3);
    const Z: &Token = &[0; 16]; // no token, as an older launcher stores

    fn relay(cfg: Config) -> RelayServer {
        RelayServer::start("127.0.0.1:0", Config { udp: false, ..cfg }, 0).unwrap()
    }

    #[test]
    fn addresses_take_the_relay_port_by_default() {
        assert_eq!(addr("192.0.2.10"), Some("192.0.2.10:47610".parse().unwrap()));
        assert_eq!(addr(" 192.0.2.10:5000 "), Some("192.0.2.10:5000".parse().unwrap()));
        assert_eq!(addr("relay.example.org"), None);
    }

    #[test]
    fn an_invite_is_kept_and_found_by_its_code_typed_any_way() {
        let srv = relay(Config::default());
        let at = srv.local_addr();
        let invite = b"PKCOOP1.eyJpZCI6IjAxMjM0NTY3ODlhYmNkZWYifQ";
        let code = store(at, invite, Z, T).unwrap();
        assert!(invites::valid(&code));
        assert_eq!(store(at, invite, Z, T).unwrap(), code, "the same invite keeps its code");
        let typed = invites::display(&code).to_lowercase();
        assert_eq!(lookup(at, &invites::normalize(&typed).unwrap(), T).unwrap(), invite);
        let other = if &code == b"222222" { *b"333333" } else { *b"222222" };
        assert_eq!(lookup(at, &other, T), Err(Error::NotFound));
        assert_eq!(srv.status().stats.invites, 1);
    }

    #[test]
    fn lookups_are_rate_limited_per_address() {
        let srv = relay(Config { invite_per_min: 3, ..Config::default() });
        let at = srv.local_addr();
        for _ in 0..3 {
            assert_eq!(lookup(at, b"222222", T), Err(Error::NotFound));
        }
        assert_eq!(lookup(at, b"222222", T), Err(Error::Limit));
        assert_eq!(store(at, b"x", Z, T), Err(Error::Limit), "stores share the budget");
    }

    #[test]
    fn a_relay_without_invites_refuses_and_a_full_store_drops_its_oldest() {
        let off = relay(Config { invites: false, ..Config::default() });
        assert_eq!(store(off.local_addr(), b"PKCOOP1.x", Z, T), Err(Error::Unsupported));
        assert_eq!(lookup(off.local_addr(), b"222222", T), Err(Error::Unsupported));
        assert_eq!(delete(off.local_addr(), b"PKCOOP1.x", Z, T), Err(Error::Unsupported));
        let full = relay(Config { invite_max: 1, ..Config::default() });
        let at = full.local_addr();
        let x = store(at, b"PKCOOP1.x", Z, T).unwrap();
        let y = store(at, b"PKCOOP1.y", Z, T).unwrap();
        assert_eq!(lookup(at, &x, T), Err(Error::NotFound));
        assert_eq!(lookup(at, &y, T).unwrap(), b"PKCOOP1.y");
    }

    #[test]
    fn only_launcher_invites_are_kept() {
        let srv = relay(Config::default());
        let at = srv.local_addr();
        for bad in [&b"x"[..], b"PKCOOP1.", b"PKCOOP1.a b", b"hello world, free storage", &[0u8; 64]] {
            assert_eq!(store(at, bad, Z, T), Err(Error::Unsupported), "{:?}", String::from_utf8_lossy(bad));
        }
        assert_eq!(srv.status().stats.invites, 0);
        assert!(store(at, b"PKCOOP1.eyJpZCI6IjAifQ==", Z, T).is_ok());
    }

    #[test]
    fn one_address_keeps_at_most_eight_codes() {
        let srv = relay(Config { invite_per_min: 100, ..Config::default() });
        let at = srv.local_addr();
        let first = store(at, b"PKCOOP1.first", Z, T).unwrap();
        for n in 1..invites::PER_ADDR {
            store(at, format!("PKCOOP1.n{}", n).as_bytes(), Z, T).unwrap();
        }
        assert_eq!(store(at, b"PKCOOP1.ninth", Z, T), Err(Error::Limit), "the ninth is refused: the launcher shows the long code");
        assert_eq!(srv.status().stats.invites, invites::PER_ADDR);
        assert_eq!(lookup(at, &first, T).unwrap(), b"PKCOOP1.first", "nothing it kept was pushed out");
        assert_eq!(store(at, b"PKCOOP1.first", Z, T), Ok(first), "one it keeps is stored again");
    }

    #[test]
    fn a_save_keeps_one_code_and_a_delete_by_its_token_drops_it() {
        let srv = relay(Config::default());
        let at = srv.local_addr();
        let t: Token = [3; 16];
        let code = store(at, b"PKCOOP1.two", &t, T).unwrap();
        // a member change: a new invite of the same save keeps the code and is what it opens now
        assert_eq!(store(at, b"PKCOOP1.three", &t, T), Ok(code));
        assert_eq!(lookup(at, &code, T).unwrap(), b"PKCOOP1.three");
        assert_eq!(srv.status().stats.invites, 1);
        // the delete sends the save's newest invite and token: every code of the save goes
        assert_eq!(delete(at, b"PKCOOP1.four", &t, T), Ok(true));
        assert_eq!(lookup(at, &code, T), Err(Error::NotFound));
    }

    #[test]
    fn a_deleted_invite_no_longer_has_a_code() {
        let srv = relay(Config::default());
        let at = srv.local_addr();
        let invite = b"PKCOOP1.eyJpZCI6IjAxMjMifQ";
        let code = store(at, invite, Z, T).unwrap();
        assert_eq!(delete(at, invite, Z, T), Ok(true));
        assert_eq!(lookup(at, &code, T), Err(Error::NotFound));
        assert_eq!(delete(at, invite, Z, T), Ok(false));
        assert_eq!(delete(at, b"not an invite", Z, T), Err(Error::Unsupported));
    }

    #[test]
    fn nobody_listening_is_unreachable() {
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let at = l.local_addr().unwrap();
        drop(l);
        assert_eq!(store(at, b"x", Z, Duration::from_millis(500)), Err(Error::Unreachable));
    }
}
