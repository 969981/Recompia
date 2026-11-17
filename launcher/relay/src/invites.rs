// SPDX-License-Identifier: GPL-3.0-or-later
//! pokopia-recomp extension: short invite codes (lib.rs "Invite codes"). The relay keeps a launcher's co-op invite (never parsed: only its
//! shape is checked, looks_like_invite) under a 6-symbol code for a few hours, so a player can read the code out instead of pasting the
//! whole invite. This file is the store and the code format; engine.rs carries them on the wire, invite_client.rs is the launcher's side.

use std::collections::HashMap;

/// The code's symbols: digits and capitals without the look-alikes (0 O, 1 I L, U V). 30^6 = 729 million codes.
pub const ALPHABET: &[u8; 30] = b"23456789ABCDEFGHJKMNPQRSTVWXYZ";
pub const CODE_LEN: usize = 6;
/// The longest invite the relay keeps (one with four players and 24-character names in any script stays under 1100 bytes).
pub const BLOB_MAX: usize = 2048;
/// How long an invite is kept after it was last stored. Short, so a code read out on a stream soon stops opening the room: it only has to
/// last until the friends have joined once (their launchers then keep the save), and opening the invite again stores it again.
pub const TTL_MS: i64 = 6 * 3600 * 1000;
/// Invites one address keeps at once: its next one replaces its oldest, so one address can never fill the store.
pub const PER_ADDR: usize = 8;
/// What every launcher invite starts with (coop.rs INVITE_PREFIX); base64 follows.
pub const PREFIX: &[u8] = b"PKCOOP1.";
/// Random draws before a store gives up (only near a full code space, which the entry cap keeps far away).
const TRIES: usize = 32;

pub type Code = [u8; CODE_LEN];

/// A typed code as the relay wants it: case does not matter, dashes and spaces are skipped. None: not six symbols of the alphabet.
pub fn normalize(text: &str) -> Option<Code> {
    let mut c = [0u8; CODE_LEN];
    let mut n = 0;
    for b in text.bytes().filter(|b| *b != b'-' && !b.is_ascii_whitespace()) {
        let b = b.to_ascii_uppercase();
        if n == CODE_LEN || !ALPHABET.contains(&b) {
            return None;
        }
        c[n] = b;
        n += 1;
    }
    (n == CODE_LEN).then_some(c)
}

/// `XXX-XXX`, as the launcher shows it.
pub fn display(c: &Code) -> String {
    let s = String::from_utf8_lossy(c);
    format!("{}-{}", &s[..3], &s[3..])
}

pub fn valid(c: &[u8]) -> bool {
    c.len() == CODE_LEN && c.iter().all(|b| ALPHABET.contains(b))
}

/// True when `blob` looks like a launcher invite: 1..=BLOB_MAX bytes, PREFIX, then base64 (URL-safe or standard, padded or not). The relay
/// keeps nothing else, so the store is no free storage for other data.
pub fn looks_like_invite(blob: &[u8]) -> bool {
    blob.len() <= BLOB_MAX
        && blob.strip_prefix(PREFIX).is_some_and(|body| {
            !body.is_empty() && body.iter().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'+' | b'/' | b'='))
        })
}

/// A code from `fill` (a random source, false when it failed): a byte below 240 picks symbol byte % 30, the rest are drawn again, so every
/// symbol is as likely. None: the source failed.
fn draw(fill: &mut dyn FnMut(&mut [u8]) -> bool) -> Option<Code> {
    let mut c = [0u8; CODE_LEN];
    let mut n = 0;
    let mut b = [0u8; 16];
    while n < CODE_LEN {
        if !fill(&mut b) {
            return None;
        }
        for x in b.iter().filter(|x| **x < 240) {
            if n < CODE_LEN {
                c[n] = ALPHABET[(*x % 30) as usize];
                n += 1;
            }
        }
    }
    Some(c)
}

/// The OS random source for draw: false when it failed (its buffer is then not used, so a failure never makes a guessable code).
pub fn os_fill(b: &mut [u8]) -> bool {
    crate::random::os_random(b).is_ok()
}

struct Entry {
    blob: Vec<u8>,
    at: i64,
    seq: u64, // the store's count when it was last stored: orders stores within one millisecond
    ip: u32,  // the address that stored it first
}

/// The invites a relay keeps: at most `max`, each for TTL_MS after it was last stored, at most PER_ADDR per address. A store always gets a
/// code: past its address's PER_ADDR the address's oldest invite goes, and in a full store the oldest of all, so flooding it takes many
/// addresses and only shortens how long codes last.
pub struct Store {
    m: HashMap<Code, Entry>,
    max: usize,
    seq: u64,
}

impl Store {
    pub fn new(max: usize) -> Store {
        Store { m: HashMap::new(), max, seq: 0 }
    }

    pub fn len(&self) -> usize {
        self.m.len()
    }

    pub fn prune(&mut self, now: i64) {
        self.m.retain(|_, e| now - e.at < TTL_MS);
    }

    /// Keeps `blob` (an invite, looks_like_invite) for address `ip` and returns its code. The same blob again gets its code back, kept
    /// TTL_MS from now (it stays its first address's). None: the random source failed (or, with a nearly full code space, no free code was
    /// drawn); nothing was dropped then.
    pub fn put(&mut self, blob: &[u8], ip: u32, now: i64, fill: &mut dyn FnMut(&mut [u8]) -> bool) -> Option<Code> {
        self.prune(now);
        self.seq += 1;
        if let Some((c, e)) = self.m.iter_mut().find(|(_, e)| e.blob == blob) {
            e.at = now;
            e.seq = self.seq;
            return Some(*c);
        }
        let mut code = None;
        for _ in 0..TRIES {
            let c = draw(fill)?;
            if !self.m.contains_key(&c) {
                code = Some(c);
                break;
            }
        }
        let c = code?;
        if self.m.values().filter(|e| e.ip == ip).count() >= PER_ADDR {
            self.drop_oldest(Some(ip));
        } else if self.m.len() >= self.max {
            self.drop_oldest(None);
        }
        self.m.insert(c, Entry { blob: blob.to_vec(), at: now, seq: self.seq, ip });
        Some(c)
    }

    /// Drops the invite stored longest ago: of address `ip`, or of all.
    fn drop_oldest(&mut self, ip: Option<u32>) {
        let old = self.m.iter().filter(|(_, e)| ip.is_none() || ip == Some(e.ip)).min_by_key(|(_, e)| (e.at, e.seq)).map(|(c, _)| *c);
        if let Some(c) = old {
            self.m.remove(&c);
        }
    }

    /// Forgets `blob` (the launcher's delete, by the invite itself: only who has it can drop its code). True: it was kept.
    pub fn remove(&mut self, blob: &[u8]) -> bool {
        let n = self.m.len();
        self.m.retain(|_, e| e.blob != blob);
        self.m.len() != n
    }

    /// The invite under `code`, while it lasts.
    pub fn get(&self, code: &Code, now: i64) -> Option<&[u8]> {
        self.m.get(code).filter(|e| now - e.at < TTL_MS).map(|e| &e.blob[..])
    }
}

/// How many stores, deletes and lookups each address may make: `rate` a minute, bursting as much. Kept apart from the relay's address table (whose
/// records are reused as soon as an address holds no link, which every invite link soon does): an address is forgotten only after a
/// minute without a request, when its budget is full again anyway, so leaving and coming back never refills it early. A full table refuses.
pub struct Limiter {
    m: HashMap<u32, Budget>,
    rate: i64,
    max: usize,
}

struct Budget {
    tok: i64, // thousandths of a request
    at: i64,
    used: i64,
}

const FORGET_MS: i64 = 60_000; // an empty budget is full again after a minute

impl Limiter {
    pub fn new(rate: i64, max: usize) -> Limiter {
        Limiter { m: HashMap::new(), rate, max }
    }

    /// Forgets the addresses quiet for a minute.
    pub fn prune(&mut self, now: i64) {
        self.m.retain(|_, b| now - b.used < FORGET_MS);
    }

    /// True takes one request from `ip`'s budget.
    pub fn ok(&mut self, ip: u32, now: i64) -> bool {
        if !self.m.contains_key(&ip) && self.m.len() >= self.max {
            self.prune(now);
            if self.m.len() >= self.max {
                return false;
            }
        }
        let rate = self.rate;
        let b = self.m.entry(ip).or_insert(Budget { tok: 1000 * rate, at: now, used: now });
        let add = (now - b.at) * rate / 60;
        if add > 0 {
            b.tok = (b.tok + add).min(1000 * rate);
            b.at = now;
        }
        b.used = now;
        if b.tok < 1000 {
            return false;
        }
        b.tok -= 1000;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_budget_refills_at_its_rate_and_is_never_forgotten_early() {
        let mut l = Limiter::new(3, 2);
        assert!((0..3).all(|_| l.ok(1, 0)));
        assert!(!l.ok(1, 0));
        assert!(!l.ok(1, 19_000), "a third of a minute: not yet one more");
        assert!(l.ok(1, 21_000));
        // two other addresses fill the table: a third is refused while both were busy within the minute, the first one kept its budget
        assert!(l.ok(2, 21_000));
        assert!(!l.ok(3, 30_000));
        assert!(!l.ok(1, 30_000));
        // after a quiet minute they are forgotten (full again anyway) and a new address gets a place
        assert!(l.ok(3, 91_000));
    }

    #[test]
    fn codes_are_typed_loosely_and_shown_with_a_dash() {
        let c = normalize(" k7q-m2x ").unwrap();
        assert_eq!(&c, b"K7QM2X");
        assert_eq!(display(&c), "K7Q-M2X");
        assert_eq!(normalize("K7Q M2X"), Some(c));
        for bad in ["", "K7QM2", "K7QM2XA", "K7QM2O", "K7QM21", "K7QM2U", "PKCOOP1.e30"] {
            assert_eq!(normalize(bad), None, "{}", bad);
        }
    }

    #[test]
    fn every_symbol_is_drawn_and_nothing_else() {
        let mut seen = [0u32; 256];
        for _ in 0..3000 {
            for b in draw(&mut os_fill).unwrap() {
                seen[b as usize] += 1;
            }
        }
        for (b, n) in seen.iter().enumerate() {
            assert_eq!(*n > 0, ALPHABET.contains(&(b as u8)), "{}", b as u8 as char);
        }
    }

    #[test]
    fn an_invite_is_kept_for_the_ttl_once_under_one_code() {
        assert_eq!(TTL_MS, 6 * 3600 * 1000);
        let mut s = Store::new(4);
        let a = s.put(b"PKCOOP1.a", 1, 1000, &mut os_fill).unwrap();
        assert_eq!(s.get(&a, 1000 + TTL_MS - 1), Some(&b"PKCOOP1.a"[..]));
        assert_eq!(s.get(&a, 1000 + TTL_MS), None);
        // the same invite again, from any address: the same code, the full TTL from now
        assert_eq!(s.put(b"PKCOOP1.a", 2, 5000, &mut os_fill), Some(a));
        assert!(s.get(&a, 1000 + TTL_MS).is_some());
        assert_eq!(s.len(), 1);
        assert_eq!(s.get(b"222222", 1000), None);
    }

    #[test]
    fn only_launcher_invites_look_like_invites() {
        assert!(looks_like_invite(b"PKCOOP1.eyJpZCI6IjAxMjM0NTY3ODlhYmNkZWYifQ"));
        assert!(looks_like_invite(b"PKCOOP1.ab-_+/cd=="));
        let big = [PREFIX,&[b'A'; BLOB_MAX - 8][..]].concat();
        assert!(looks_like_invite(&big));
        let too_big = [&big[..], b"A"].concat();
        for bad in [&b""[..], b"x", b"PKCOOP1.", b"PKCOOP2.abc", b"pkcoop1.abc", b"PKCOOP1.ab cd", b"PKCOOP1.ab\n", b"PKCOOP1.\xff", &too_big] {
            assert!(!looks_like_invite(bad), "{:?}", String::from_utf8_lossy(bad));
        }
    }

    fn inv(n: usize) -> Vec<u8> {
        format!("PKCOOP1.{}", n).into_bytes()
    }

    #[test]
    fn one_address_keeps_at_most_per_addr_and_its_next_replaces_its_oldest() {
        let mut s = Store::new(100);
        let other = s.put(&inv(999), 2, 0, &mut os_fill).unwrap();
        let codes: Vec<Code> = (0..PER_ADDR).map(|n| s.put(&inv(n), 1, 10 + n as i64, &mut os_fill).unwrap()).collect();
        assert_eq!(s.len(), PER_ADDR + 1);
        // a flood from address 1: its own oldest go, the other address's invite stays
        for n in PER_ADDR..PER_ADDR + 50 {
            s.put(&inv(n), 1, 10 + n as i64, &mut os_fill).unwrap();
        }
        assert_eq!(s.len(), PER_ADDR + 1);
        assert!(s.get(&other, 100).is_some());
        assert!(codes.iter().all(|c| s.get(c, 100).is_none()));
        // re-storing keeps an invite alive but counts nothing new
        let last = s.put(&inv(PER_ADDR + 49), 1, 200, &mut os_fill).unwrap();
        assert!(s.get(&last, 200).is_some());
        assert_eq!(s.len(), PER_ADDR + 1);
    }

    #[test]
    fn stores_within_one_millisecond_keep_their_order() {
        let mut s = Store::new(100);
        let first = s.put(&inv(0), 1, 7, &mut os_fill).unwrap();
        let second = s.put(&inv(1), 1, 7, &mut os_fill).unwrap();
        for n in 2..=PER_ADDR {
            s.put(&inv(n), 1, 7, &mut os_fill).unwrap();
        }
        assert_eq!(s.get(&first, 7), None);
        assert!(s.get(&second, 7).is_some());
    }

    #[test]
    fn a_full_store_drops_its_oldest_invite() {
        let mut s = Store::new(2);
        let a = s.put(&inv(0), 1, 0, &mut os_fill).unwrap();
        let b = s.put(&inv(1), 2, 10, &mut os_fill).unwrap();
        let c = s.put(&inv(2), 3, 20, &mut os_fill).unwrap();
        assert_eq!(s.len(), 2);
        assert_eq!(s.get(&a, 20), None);
        assert!(s.get(&b, 20).is_some() && s.get(&c, 20).is_some());
        // a refreshed invite is younger than one stored after it
        assert_eq!(s.put(&inv(1), 2, 30, &mut os_fill), Some(b));
        s.put(&inv(3), 4, 40, &mut os_fill).unwrap();
        assert!(s.get(&b, 40).is_some());
        assert_eq!(s.get(&c, 40), None);
    }

    #[test]
    fn a_failing_random_source_stores_nothing() {
        let mut s = Store::new(2);
        let a = s.put(&inv(0), 1, 0, &mut os_fill).unwrap();
        s.put(&inv(1), 1, 0, &mut os_fill).unwrap();
        // zeros it never fills in would be code 222222: refused instead, and nothing is dropped for it
        let mut fail = |b: &mut [u8]| {
            b.fill(0);
            false
        };
        assert_eq!(s.put(&inv(2), 1, 10, &mut fail), None);
        assert_eq!(s.len(), 2);
        assert!(s.get(&a, 10).is_some());
        assert_eq!(s.get(b"222222", 10), None);
    }

    #[test]
    fn an_invite_is_removed_by_its_own_text() {
        let mut s = Store::new(4);
        let a = s.put(&inv(0), 1, 0, &mut os_fill).unwrap();
        assert!(!s.remove(&inv(1)));
        assert!(s.remove(&inv(0)));
        assert_eq!(s.get(&a, 0), None);
        assert!(!s.remove(&inv(0)));
    }

    #[test]
    fn a_taken_code_is_drawn_again() {
        let mut s = Store::new(8);
        // a source that gives zeros (code 222222) twice, then ones (333333)
        let mut calls = 0;
        let mut fill = |b: &mut [u8]| {
            b.fill(if calls < 2 { 0 } else { 1 });
            calls += 1;
            true
        };
        assert_eq!(&s.put(&inv(0), 1, 0, &mut fill).unwrap(), b"222222");
        assert_eq!(&s.put(&inv(1), 1, 0, &mut fill).unwrap(), b"333333");
    }
}
