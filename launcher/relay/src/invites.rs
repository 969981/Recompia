// SPDX-License-Identifier: GPL-3.0-or-later
//! pokopia-recomp extension: short invite codes (lib.rs "Invite codes"). The relay keeps a launcher's co-op invite (an opaque blob, never
//! parsed) under a 6-symbol code for a day, so a player can read the code out instead of pasting the whole invite. This file is the store
//! and the code format; engine.rs carries them on the wire, invite_client.rs is the launcher's side.

use std::collections::HashMap;

/// The code's symbols: digits and capitals without the look-alikes (0 O, 1 I L, U V). 30^6 = 729 million codes.
pub const ALPHABET: &[u8; 30] = b"23456789ABCDEFGHJKMNPQRSTVWXYZ";
pub const CODE_LEN: usize = 6;
/// The longest invite the relay keeps (one with four players and 24-character names in any script stays under 1100 bytes).
pub const BLOB_MAX: usize = 2048;
/// How long an invite is kept after it was last stored.
pub const TTL_MS: i64 = 24 * 3600 * 1000;
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

/// A code from `fill` (a random source): a byte below 240 picks symbol byte % 30, the rest are drawn again, so every symbol is as likely.
fn draw(fill: &mut dyn FnMut(&mut [u8])) -> Code {
    let mut c = [0u8; CODE_LEN];
    let mut n = 0;
    let mut b = [0u8; 16];
    while n < CODE_LEN {
        fill(&mut b);
        for x in b.iter().filter(|x| **x < 240) {
            if n < CODE_LEN {
                c[n] = ALPHABET[(*x % 30) as usize];
                n += 1;
            }
        }
    }
    c
}

pub fn os_fill(b: &mut [u8]) {
    let _ = crate::random::os_random(b); // never fails on a running OS; a zero buffer would only make the store retry
}

struct Entry {
    blob: Vec<u8>,
    at: i64,
}

/// The invites a relay keeps: at most `max`, each for TTL_MS after it was stored. A full store refuses (the launcher keeps the long code)
/// rather than drop someone's code.
pub struct Store {
    m: HashMap<Code, Entry>,
    max: usize,
}

impl Store {
    pub fn new(max: usize) -> Store {
        Store { m: HashMap::new(), max }
    }

    pub fn len(&self) -> usize {
        self.m.len()
    }

    pub fn prune(&mut self, now: i64) {
        self.m.retain(|_, e| now - e.at < TTL_MS);
    }

    /// Keeps `blob` (1..=BLOB_MAX bytes) and returns its code. The same blob again gets its code back, kept a day from now. None: full.
    pub fn put(&mut self, blob: &[u8], now: i64, fill: &mut dyn FnMut(&mut [u8])) -> Option<Code> {
        self.prune(now);
        if let Some((c, e)) = self.m.iter_mut().find(|(_, e)| e.blob == blob) {
            e.at = now;
            return Some(*c);
        }
        if self.m.len() >= self.max {
            return None;
        }
        let c = (0..TRIES).map(|_| draw(fill)).find(|c| !self.m.contains_key(c))?;
        self.m.insert(c, Entry { blob: blob.to_vec(), at: now });
        Some(c)
    }

    /// The invite under `code`, while it lasts.
    pub fn get(&self, code: &Code, now: i64) -> Option<&[u8]> {
        self.m.get(code).filter(|e| now - e.at < TTL_MS).map(|e| &e.blob[..])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
            for b in draw(&mut os_fill) {
                seen[b as usize] += 1;
            }
        }
        for (b, n) in seen.iter().enumerate() {
            assert_eq!(*n > 0, ALPHABET.contains(&(b as u8)), "{}", b as u8 as char);
        }
    }

    #[test]
    fn an_invite_is_kept_a_day_once_under_one_code() {
        let mut s = Store::new(4);
        let a = s.put(b"PKCOOP1.a", 1000, &mut os_fill).unwrap();
        assert_eq!(s.get(&a, 1000 + TTL_MS - 1), Some(&b"PKCOOP1.a"[..]));
        assert_eq!(s.get(&a, 1000 + TTL_MS), None);
        // the same invite again: the same code, a day from now
        assert_eq!(s.put(b"PKCOOP1.a", 5000, &mut os_fill), Some(a));
        assert!(s.get(&a, 1000 + TTL_MS).is_some());
        assert_eq!(s.len(), 1);
        assert_eq!(s.get(b"222222", 1000), None);
    }

    #[test]
    fn a_full_store_refuses_until_an_invite_expires() {
        let mut s = Store::new(2);
        s.put(b"a", 0, &mut os_fill).unwrap();
        s.put(b"b", 10, &mut os_fill).unwrap();
        assert_eq!(s.put(b"c", 20, &mut os_fill), None);
        assert!(s.put(b"c", TTL_MS + 1, &mut os_fill).is_some(), "the first one expired");
    }

    #[test]
    fn a_taken_code_is_drawn_again() {
        let mut s = Store::new(8);
        // a source that gives zeros (code 222222) twice, then ones (333333)
        let mut calls = 0;
        let mut fill = |b: &mut [u8]| {
            b.fill(if calls < 2 { 0 } else { 1 });
            calls += 1;
        };
        assert_eq!(&s.put(b"a", 0, &mut fill).unwrap(), b"222222");
        assert_eq!(&s.put(b"b", 0, &mut fill).unwrap(), b"333333");
    }
}
