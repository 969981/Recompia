// SPDX-License-Identifier: GPL-3.0-or-later
//! Recompia extension: short invite codes (lib.rs "Invite codes"). The relay keeps a launcher's co-op invite (never parsed: only its
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
/// Invites one address keeps at once: past it a new one is refused ("limit"; the launcher then shows the long invite), so one address can
/// never fill the store, and players behind one shared address never push each other's codes out.
pub const PER_ADDR: usize = 8;
/// What every launcher invite starts with (coop.rs INVITE_PREFIX); base64 follows.
pub const PREFIX: &[u8] = b"PKCOOP1.";
/// Random draws before a store gives up (only near a full code space, which the entry cap keeps far away).
const TRIES: usize = 32;

pub type Code = [u8; CODE_LEN];
/// What a launcher sends with an invite to name its co-op save (coop_code.rs token: a hash of the save's group id and private room code), so
/// every invite of one save, which changes with each member change, is one entry: storing a newer one keeps the code, and a delete drops it.
/// All zeros (older launchers): none, the invite itself names the entry.
pub type Token = [u8; 16];

fn token_of(t: &Token) -> Option<Token> {
    (t != &[0u8; 16]).then_some(*t)
}

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
    token: Option<Token>,
    at: i64,
    seq: u64, // the store's count when it was last stored: orders stores within one millisecond
    ip: u32,  // the address that stored it first
}

/// The invites a relay keeps: at most `max`, each for TTL_MS after it was last stored, at most PER_ADDR per address (past that a new one is
/// refused). A full store makes room by dropping the oldest invite of the address that keeps the most, so flooding it takes many
/// addresses, never pushes out the codes of an address that keeps few, and only shortens how long codes last.
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

    /// Keeps `blob` (an invite, looks_like_invite) of save `token` for address `ip` and returns its code. The same blob again, or another
    /// with the same token (the save's newer invite, which then replaces it), gets its code back, kept TTL_MS from now (it stays its first
    /// address's). None, with nothing dropped: the address already keeps PER_ADDR invites, or the random source failed (or, with a nearly
    /// full code space, no free code was drawn).
    pub fn put(&mut self, blob: &[u8], token: &Token, ip: u32, now: i64, fill: &mut dyn FnMut(&mut [u8]) -> bool) -> Option<Code> {
        self.prune(now);
        self.seq += 1;
        let token = token_of(token);
        if let Some((c, e)) = self.m.iter_mut().find(|(_, e)| e.blob == blob || (token.is_some() && e.token == token)) {
            if e.blob != blob {
                e.blob = blob.to_vec();
            }
            e.token = token.or(e.token);
            e.at = now;
            e.seq = self.seq;
            return Some(*c);
        }
        if self.m.values().filter(|e| e.ip == ip).count() >= PER_ADDR {
            return None;
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
        if self.m.len() >= self.max {
            self.drop_from_fullest();
        }
        self.m.insert(c, Entry { blob: blob.to_vec(), token, at: now, seq: self.seq, ip });
        Some(c)
    }

    /// Drops the invite stored longest ago of the address that keeps the most (of those, the one whose oldest invite is oldest).
    fn drop_from_fullest(&mut self) {
        let mut per: HashMap<u32, (usize, (i64, u64))> = HashMap::new();
        for e in self.m.values() {
            let p = per.entry(e.ip).or_insert((0, (e.at, e.seq)));
            p.0 += 1;
            p.1 = p.1.min((e.at, e.seq));
        }
        let fullest = per.iter().max_by(|a, b| a.1 .0.cmp(&b.1 .0).then(b.1 .1.cmp(&a.1 .1))).map(|(ip, _)| *ip);
        let old = self.m.iter().filter(|(_, e)| Some(e.ip) == fullest).min_by_key(|(_, e)| (e.at, e.seq)).map(|(c, _)| *c);
        if let Some(c) = old {
            self.m.remove(&c);
        }
    }

    /// Forgets `blob` and every invite of save `token` (the launcher's delete, by the invite itself and its save's token: only who has
    /// them can drop the codes). True: one was kept.
    pub fn remove(&mut self, blob: &[u8], token: &Token) -> bool {
        let token = token_of(token);
        let n = self.m.len();
        self.m.retain(|_, e| e.blob != blob && (token.is_none() || e.token != token));
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

    const Z: &Token = &[0; 16]; // no token: an older launcher's store

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
        let a = s.put(b"PKCOOP1.a", Z, 1, 1000, &mut os_fill).unwrap();
        assert_eq!(s.get(&a, 1000 + TTL_MS - 1), Some(&b"PKCOOP1.a"[..]));
        assert_eq!(s.get(&a, 1000 + TTL_MS), None);
        // the same invite again, from any address: the same code, the full TTL from now
        assert_eq!(s.put(b"PKCOOP1.a", Z, 2, 5000, &mut os_fill), Some(a));
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
    fn one_address_keeps_at_most_per_addr_and_its_next_is_refused() {
        let mut s = Store::new(100);
        let other = s.put(&inv(999), Z, 2, 0, &mut os_fill).unwrap();
        let codes: Vec<Code> = (0..PER_ADDR).map(|n| s.put(&inv(n), Z, 1, 10 + n as i64, &mut os_fill).unwrap()).collect();
        assert_eq!(s.len(), PER_ADDR + 1);
        // a flood from address 1 (or many players behind one shared address): refused, nothing it or anyone kept is pushed out
        for n in PER_ADDR..PER_ADDR + 50 {
            assert_eq!(s.put(&inv(n), Z, 1, 10 + n as i64, &mut os_fill), None);
        }
        assert_eq!(s.len(), PER_ADDR + 1);
        assert!(s.get(&other, 100).is_some());
        assert!(codes.iter().all(|c| s.get(c, 100).is_some()));
        // re-storing one it keeps, or a newer invite of a save it keeps, still works at the cap and counts nothing new
        assert_eq!(s.put(&inv(0), Z, 1, 200, &mut os_fill), Some(codes[0]));
        let t: Token = [7; 16];
        let mut s2 = Store::new(100);
        let first = s2.put(&inv(0), &t, 1, 0, &mut os_fill).unwrap();
        for n in 1..PER_ADDR {
            s2.put(&inv(n), Z, 1, 0, &mut os_fill).unwrap();
        }
        assert_eq!(s2.put(&inv(50), &t, 1, 10, &mut os_fill), Some(first));
        assert_eq!(s2.get(&first, 10), Some(&inv(50)[..]));
        assert_eq!(s.len(), PER_ADDR + 1);
    }

    #[test]
    fn stores_within_one_millisecond_keep_their_order() {
        let mut s = Store::new(3);
        let first = s.put(&inv(0), Z, 1, 7, &mut os_fill).unwrap();
        let second = s.put(&inv(1), Z, 1, 7, &mut os_fill).unwrap();
        s.put(&inv(2), Z, 2, 7, &mut os_fill).unwrap();
        s.put(&inv(3), Z, 3, 7, &mut os_fill).unwrap();
        assert_eq!(s.get(&first, 7), None);
        assert!(s.get(&second, 7).is_some());
    }

    #[test]
    fn a_full_store_drops_the_oldest_of_the_address_keeping_the_most() {
        let mut s = Store::new(4);
        let a = s.put(&inv(0), Z, 1, 0, &mut os_fill).unwrap();
        let b1 = s.put(&inv(1), Z, 2, 10, &mut os_fill).unwrap();
        let b2 = s.put(&inv(2), Z, 2, 20, &mut os_fill).unwrap();
        let c = s.put(&inv(3), Z, 3, 30, &mut os_fill).unwrap();
        // address 2 keeps the most: its oldest goes, not address 1's older one
        let d = s.put(&inv(4), Z, 4, 40, &mut os_fill).unwrap();
        assert_eq!(s.len(), 4);
        assert_eq!(s.get(&b1, 40), None);
        assert!([a, b2, c, d].iter().all(|x| s.get(x, 40).is_some()));
        // all keep one: the oldest of all goes; a refreshed invite is younger than one stored after it
        assert_eq!(s.put(&inv(0), Z, 1, 50, &mut os_fill), Some(a));
        s.put(&inv(5), Z, 5, 60, &mut os_fill).unwrap();
        assert_eq!(s.get(&b2, 60), None);
        assert!(s.get(&a, 60).is_some());
        s.put(&inv(6), Z, 6, 70, &mut os_fill).unwrap();
        assert_eq!(s.get(&c, 70), None);
    }

    #[test]
    fn a_save_keeps_one_code_through_its_invites_and_a_delete_drops_it() {
        let t: Token = [9; 16];
        let mut s = Store::new(16);
        let code = s.put(&inv(0), &t, 1, 0, &mut os_fill).unwrap();
        // a member change makes a new invite: same token, same code, now the newer invite (from any address)
        assert_eq!(s.put(&inv(1), &t, 2, 10, &mut os_fill), Some(code));
        assert_eq!(s.get(&code, 10), Some(&inv(1)[..]));
        assert_eq!(s.len(), 1);
        // an invite an older launcher stored without a token is taken over by the same invite with one
        let old = s.put(&inv(7), Z, 1, 20, &mut os_fill).unwrap();
        let u: Token = [5; 16];
        assert_eq!(s.put(&inv(7), &u, 1, 30, &mut os_fill), Some(old));
        assert_eq!(s.put(&inv(8), &u, 1, 40, &mut os_fill), Some(old));
        // a delete by the token drops the save's code whatever invite it sends; another save's stays
        assert!(s.remove(&inv(99), &t));
        assert_eq!(s.get(&code, 40), None);
        assert!(s.get(&old, 40).is_some());
        assert!(!s.remove(&inv(99), &t));
        // without a token only that very invite goes
        let plain = s.put(&inv(20), Z, 1, 50, &mut os_fill).unwrap();
        assert!(!s.remove(&inv(21), Z));
        assert!(s.remove(&inv(20), Z));
        assert_eq!(s.get(&plain, 50), None);
    }

    #[test]
    fn a_failing_random_source_stores_nothing() {
        let mut s = Store::new(2);
        let a = s.put(&inv(0), Z, 1, 0, &mut os_fill).unwrap();
        s.put(&inv(1), Z, 1, 0, &mut os_fill).unwrap();
        // zeros it never fills in would be code 222222: refused instead, and nothing is dropped for it
        let mut fail = |b: &mut [u8]| {
            b.fill(0);
            false
        };
        assert_eq!(s.put(&inv(2), Z, 1, 10, &mut fail), None);
        assert_eq!(s.len(), 2);
        assert!(s.get(&a, 10).is_some());
        assert_eq!(s.get(b"222222", 10), None);
    }

    #[test]
    fn an_invite_is_removed_by_its_own_text() {
        let mut s = Store::new(4);
        let a = s.put(&inv(0), Z, 1, 0, &mut os_fill).unwrap();
        assert!(!s.remove(&inv(1), Z));
        assert!(s.remove(&inv(0), Z));
        assert_eq!(s.get(&a, 0), None);
        assert!(!s.remove(&inv(0), Z));
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
        assert_eq!(&s.put(&inv(0), Z, 1, 0, &mut fill).unwrap(), b"222222");
        assert_eq!(&s.put(&inv(1), Z, 1, 0, &mut fill).unwrap(), b"333333");
    }
}
