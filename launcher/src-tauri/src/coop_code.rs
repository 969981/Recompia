// SPDX-License-Identifier: GPL-3.0-or-later
//! Short invite codes: the relay keeps a co-op invite (coop.rs) for 6 hours under a code a player can read out, `XXX-XXX`
//! (pokopia_relay::invites; launcher/relay/src/lib.rs "Invite codes"). The long invite stays the real one: a short code only points at it,
//! so a relay that is old, down or full costs the short code and nothing else.

use sha2::{Digest, Sha256};
use std::net::SocketAddr;
use std::time::Duration;

use crate::coop::Profile;
use crate::relay::invite_client::{self, Error};
use crate::relay::invites::{self, Token};

/// Each step of a store or a lookup (connect, send, answer): the page waits on it, so a dead relay only costs this.
const TIMEOUT: Duration = Duration::from_secs(4);

/// What names save `p` to the relay (invites::Token): the same through every invite of the save (they change with each member change),
/// so storing a newer one keeps its code and a delete drops them all. A hash of the group id and the private room code: only the members
/// can make it, and it gives away neither.
pub fn token(p: &Profile) -> Token {
    let mut h = Sha256::new();
    h.update(b"recompia coop invite token\0");
    h.update(p.id.as_bytes());
    h.update(b"\0");
    h.update(p.room_code.as_bytes());
    let mut t = [0u8; 16];
    t.copy_from_slice(&h.finalize()[..16]);
    t
}

/// Relay `codes` (the one joiners' launchers ask) when it is the relay save `p` plays on, the only one its codes are kept on: the invite
/// holds the room's private code, so it is never handed to another relay.
fn codes_relay(p: &Profile, codes: &str) -> Option<SocketAddr> {
    let at = invite_client::addr(codes)?;
    (invite_client::addr(&p.relay)? == at).then_some(at)
}

/// The short code for `invite` of save `p`, kept on relay `codes` (codes_relay). None when there is none for any reason (one is "limit":
/// this PC's address keeps the relay's most codes already). Each call (each time the invite dialog opens) keeps the code 6 hours from then.
pub fn shorten(p: &Profile, codes: &str, invite: &str) -> Option<String> {
    let at = codes_relay(p, codes)?;
    invite_client::store(at, invite.as_bytes(), &token(p), TIMEOUT).ok().map(|c| invites::display(&c))
}

/// Drops every short code of save `p` from relay `codes` (codes_relay, as shorten), so none of them opens the room: a co-op save was
/// deleted. `invite` is its newest invite. Best effort: an older relay, or none, keeps them until they expire.
pub fn forget(p: &Profile, codes: &str, invite: &str) {
    if let Some(at) = codes_relay(p, codes) {
        let _ = invite_client::delete(at, invite.as_bytes(), &token(p), TIMEOUT);
    }
}

/// What the player pasted or typed under Join: a short code is looked up on relay `codes` and its long invite comes back; any other text
/// comes back as it is, for coop.rs to read as an invite or refuse.
pub fn resolve(codes: &str, text: &str) -> Result<String, String> {
    let Some(code) = invites::normalize(text) else { return Ok(text.trim().to_string()) };
    let at = invite_client::addr(codes).ok_or_else(|| UNREACHABLE.to_string())?;
    match invite_client::lookup(at, &code, TIMEOUT) {
        Ok(b) => String::from_utf8(b).map_err(|_| "This is not a co-op invite.".to_string()),
        Err(Error::NotFound) => Err(NOT_FOUND.into()),
        Err(Error::Limit) => Err("Too many codes were tried from this PC. Wait a minute, then try again.".into()),
        Err(Error::Unsupported | Error::Unreachable) => Err(UNREACHABLE.into()),
    }
}

/// resolve's answer when no invite has the code: the only one the page keeps for that text (the others may pass on a retry).
pub const NOT_FOUND: &str = "No invite has this code. Codes last 6 hours: ask for a new one, or for the long code.";
const UNREACHABLE: &str = "The relay could not look up this code. Ask your friend for the long code instead.";

#[cfg(test)]
mod tests {
    use super::*;
    use crate::relay::{Config, RelayServer};

    fn save(relay: &str, id: &str, room_code: &str) -> Profile {
        Profile { version: 1, id: id.into(), name: "Island".into(), account_uid: "0".repeat(32), room_code: room_code.into(), relay: relay.into(), members: vec![], me: String::new(), split: false }
    }

    const ID: &str = "0123456789abcdef";
    const ROOM: &str = "ABCD-EFGH-JKMN-PQRS";

    #[test]
    fn a_short_code_brings_the_long_invite_back() {
        let srv = RelayServer::start("127.0.0.1:0", Config { udp: false, ..Config::default() }, 0).unwrap();
        let at = srv.local_addr().to_string();
        let p = save(&at, ID, ROOM);
        let elsewhere = save("192.0.2.10:47610", ID, ROOM);
        let long = "PKCOOP1.eyJpZCI6IjAxMjM0NTY3ODlhYmNkZWYifQ";
        let short = shorten(&p, &at, long).unwrap();
        assert_eq!(short.len(), 7);
        assert_eq!(resolve(&at, &format!("  {} ", short.to_lowercase().replace('-', " "))).unwrap(), long);
        // the long invite passes through untouched, without asking the relay
        assert_eq!(resolve("192.0.2.1:9", &format!(" {}\n", long)).unwrap(), long);
        // another relay than the save's gets nothing
        assert_eq!(shorten(&elsewhere, &at, long), None);
        // a member change makes a new invite: the code stays and opens the newer one
        let newer = "PKCOOP1.eyJpZCI6IjAxMjM0NTY3ODlhYmNkZWYiLCJuIjoyfQ";
        assert_eq!(shorten(&p, &at, newer).as_deref(), Some(short.as_str()));
        assert_eq!(resolve(&at, &short).unwrap(), newer);
        // forget, as shorten, only asks the relay the save plays on
        forget(&elsewhere, &at, newer);
        assert_eq!(resolve(&at, &short).unwrap(), newer);
        // a deleted save's code no longer opens it, whichever of its invites the delete sends
        forget(&p, &at, long);
        assert_eq!(resolve(&at, &short).unwrap_err(), NOT_FOUND);
    }

    #[test]
    fn the_token_names_the_save_and_not_its_relay() {
        let a = token(&save("", ID, ROOM));
        assert_eq!(a, token(&save("192.0.2.1", ID, ROOM)));
        assert_ne!(a, token(&save("", ID, "ABCD-EFGH-JKMN-PQRT")));
        assert_ne!(a, token(&save("", "0123456789abcdee", ROOM)));
        assert_ne!(a, [0u8; 16], "never the relay's no-token");
    }

    #[test]
    fn no_relay_means_no_short_code_and_a_plain_error() {
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let at = l.local_addr().unwrap().to_string();
        drop(l);
        assert_eq!(shorten(&save(&at, ID, ROOM), &at, "PKCOOP1.x"), None);
        assert_eq!(resolve(&at, "K7Q-M2X").unwrap_err(), UNREACHABLE);
    }
}
