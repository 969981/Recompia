// SPDX-License-Identifier: GPL-3.0-or-later
//! SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and HKDF (RFC 5869), only for the room list: the relay checks that a listed room code is the
//! one its room id comes from (coop_auth.c croom_keys, the same derivation every player makes). The standard library has no hash; this is small.

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

#[derive(Clone)]
struct Sha256 {
    h: [u32; 8],
    buf: [u8; 64],
    n: usize,
    len: u64,
}

impl Sha256 {
    fn new() -> Self {
        Sha256 { h: [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19], buf: [0; 64], n: 0, len: 0 }
    }
    fn block(&mut self) {
        let mut w = [0u32; 64];
        for (x, b) in w.iter_mut().zip(self.buf.chunks_exact(4)) {
            *x = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.h;
        for i in 0..64 {
            let t1 = h
                .wrapping_add(e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25))
                .wrapping_add((e & f) ^ (!e & g))
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let t2 = (a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22)).wrapping_add((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (x, y) in self.h.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *x = x.wrapping_add(y);
        }
    }
    fn update(&mut self, mut d: &[u8]) {
        self.len += d.len() as u64;
        while !d.is_empty() {
            let k = (64 - self.n).min(d.len());
            self.buf[self.n..self.n + k].copy_from_slice(&d[..k]);
            self.n += k;
            d = &d[k..];
            if self.n == 64 {
                self.block();
                self.n = 0;
            }
        }
    }
    fn finish(mut self) -> [u8; 32] {
        let bits = self.len * 8;
        self.update(&[0x80]);
        while self.n != 56 {
            self.update(&[0]);
        }
        self.update(&bits.to_be_bytes());
        let mut out = [0u8; 32];
        for (i, v) in self.h.iter().enumerate() {
            out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
        }
        out
    }
}

/// HMAC-SHA256 of the parts, one after another.
pub fn hmac(key: &[u8], parts: &[&[u8]]) -> [u8; 32] {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        let mut s = Sha256::new();
        s.update(key);
        k[..32].copy_from_slice(&s.finish());
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let (mut ipad, mut opad) = ([0x36u8; 64], [0x5cu8; 64]);
    for i in 0..64 {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }
    let mut s = Sha256::new();
    s.update(&ipad);
    for p in parts {
        s.update(p);
    }
    let inner = s.finish();
    let mut s = Sha256::new();
    s.update(&opad);
    s.update(&inner);
    s.finish()
}

/// HKDF-Expand of at most 32 bytes (one block): T(1) = HMAC(prk, info | 1).
pub fn expand32(prk: &[u8; 32], info: &[u8], out: &mut [u8]) {
    let t = hmac(prk, &[info, &[1u8]]);
    out.copy_from_slice(&t[..out.len()]);
}

/// The relay room id of a normalized room code: HKDF-SHA256, salt "pokopia-coop room v1", info "relay room id", 16 bytes (croom_keys).
pub fn room_id(code: &[u8]) -> [u8; 16] {
    let prk = hmac(b"pokopia-coop room v1", &[code]);
    let mut id = [0u8; 16];
    expand32(&prk, b"relay room id", &mut id);
    id
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    #[test]
    fn sha256_vectors() {
        let mut s = Sha256::new();
        s.update(b"abc");
        assert_eq!(s.finish().to_vec(), hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        let mut s = Sha256::new();
        s.update(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
        assert_eq!(s.finish().to_vec(), hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }

    #[test]
    fn hmac_rfc4231_case2() {
        let t = hmac(b"Jefe", &[b"what do ya want ", b"for nothing?"]);
        assert_eq!(t.to_vec(), hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    }

    #[test]
    fn hkdf_rfc5869_case1() {
        let prk = hmac(&hex("000102030405060708090a0b0c"), &[&hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")]);
        assert_eq!(prk.to_vec(), hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
        let mut okm = [0u8; 32];
        expand32(&prk, &hex("f0f1f2f3f4f5f6f7f8f9"), &mut okm);
        assert_eq!(okm.to_vec(), hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"));
    }
}
