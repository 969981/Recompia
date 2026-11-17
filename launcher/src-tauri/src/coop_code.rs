// SPDX-License-Identifier: GPL-3.0-or-later
//! Short invite codes: the relay keeps a co-op invite (coop.rs) for 6 hours under a code a player can read out, `XXX-XXX`
//! (pokopia_relay::invites; launcher/relay/src/lib.rs "Invite codes"). The long invite stays the real one: a short code only points at it,
//! so a relay that is old, down or full costs the short code and nothing else.

use std::time::Duration;

use crate::relay::invite_client::{self, Error};
use crate::relay::invites;

/// Each step of a store or a lookup (connect, send, answer): the page waits on it, so a dead relay only costs this.
const TIMEOUT: Duration = Duration::from_secs(4);

/// The short code for `invite`, kept on relay `codes` (the one joiners' launchers ask). Only when that is the relay the save plays on:
/// the invite holds the room's private code, so it is never handed to another relay. None when there is none for any reason. Each call
/// (each time the invite dialog opens) keeps the code 6 hours from then.
pub fn shorten(save_relay: &str, codes: &str, invite: &str) -> Option<String> {
    let at = invite_client::addr(codes)?;
    if invite_client::addr(save_relay)? != at {
        return None;
    }
    invite_client::store(at, invite.as_bytes(), TIMEOUT).ok().map(|c| invites::display(&c))
}

/// Drops `invite`'s short code from the save's relay (the only one shorten stores on), so it stops opening the room: a co-op save was
/// deleted. Best effort: an older relay, or none, keeps it until it expires.
pub fn forget(save_relay: &str, invite: &str) {
    if let Some(at) = invite_client::addr(save_relay) {
        let _ = invite_client::delete(at, invite.as_bytes(), TIMEOUT);
    }
}

/// What the player pasted or typed under Join: a short code is looked up on relay `codes` and its long invite comes back; any other text
/// comes back as it is, for coop.rs to read as an invite or refuse.
pub fn resolve(codes: &str, text: &str) -> Result<String, String> {
    let Some(code) = invites::normalize(text) else { return Ok(text.trim().to_string()) };
    let at = invite_client::addr(codes).ok_or_else(|| UNREACHABLE.to_string())?;
    match invite_client::lookup(at, &code, TIMEOUT) {
        Ok(b) => String::from_utf8(b).map_err(|_| "This is not a co-op invite.".to_string()),
        Err(Error::NotFound) => Err("No invite has this code. Codes last 6 hours: ask for a new one, or for the long code.".into()),
        Err(Error::Limit) => Err("Too many codes were tried from this PC. Wait a minute, then try again.".into()),
        Err(Error::Unsupported | Error::Unreachable) => Err(UNREACHABLE.into()),
    }
}

const UNREACHABLE: &str = "The relay could not look up this code. Ask your friend for the long code instead.";

#[cfg(test)]
mod tests {
    use super::*;
    use crate::relay::{Config, RelayServer};

    #[test]
    fn a_short_code_brings_the_long_invite_back() {
        let srv = RelayServer::start("127.0.0.1:0", Config { udp: false, ..Config::default() }, 0).unwrap();
        let at = srv.local_addr().to_string();
        let long = "PKCOOP1.eyJpZCI6IjAxMjM0NTY3ODlhYmNkZWYifQ";
        let short = shorten(&at, &at, long).unwrap();
        assert_eq!(short.len(), 7);
        assert_eq!(resolve(&at, &format!("  {} ", short.to_lowercase().replace('-', " "))).unwrap(), long);
        // the long invite passes through untouched, without asking the relay
        assert_eq!(resolve("192.0.2.1:9", &format!(" {}\n", long)).unwrap(), long);
        // another relay than the save's gets nothing
        assert_eq!(shorten("192.0.2.10:47610", &at, long), None);
        // a deleted save's code no longer opens it
        forget(&at, long);
        assert!(resolve(&at, &short).unwrap_err().starts_with("No invite has this code."));
    }

    #[test]
    fn no_relay_means_no_short_code_and_a_plain_error() {
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let at = l.local_addr().unwrap().to_string();
        drop(l);
        assert_eq!(shorten(&at, &at, "PKCOOP1.x"), None);
        assert_eq!(resolve(&at, "K7Q-M2X").unwrap_err(), UNREACHABLE);
    }
}
