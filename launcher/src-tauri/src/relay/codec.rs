// SPDX-License-Identifier: GPL-3.0-or-later
//! Frame codec: `u32 len` (LE, bytes after this field) + `u8 type` + payload, and bounds-checked field readers/writers.
//! Nothing here panics on bad input: every read checks the remaining length first.

/// Largest `len` accepted or produced (type byte + payload).
pub const MAX_FRAME: usize = 8192;

#[derive(Debug, PartialEq, Eq)]
pub enum FrameError {
    /// len 0: no type byte.
    Empty,
    /// len over MAX_FRAME (rejected from the header alone, the body is never waited for).
    TooLong(u32),
}

/// Splits the first frame off `buf`. Ok(None): more bytes needed. Ok(Some((type, payload, used))).
pub fn split(buf: &[u8]) -> Result<Option<(u8, &[u8], usize)>, FrameError> {
    if buf.len() < 4 {
        return Ok(None);
    }
    let len = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
    if len == 0 {
        return Err(FrameError::Empty);
    }
    if len as usize > MAX_FRAME {
        return Err(FrameError::TooLong(len));
    }
    let end = 4 + len as usize;
    if buf.len() < end {
        return Ok(None);
    }
    Ok(Some((buf[4], &buf[5..end], end)))
}

/// Payload ran short, had trailing bytes, or a field broke its rule.
#[derive(Debug, PartialEq, Eq)]
pub struct BadFrame;

/// Cursor over a payload.
pub struct In<'a>(&'a [u8]);

impl<'a> In<'a> {
    pub fn new(b: &'a [u8]) -> Self {
        In(b)
    }
    pub fn bytes(&mut self, n: usize) -> Result<&'a [u8], BadFrame> {
        if self.0.len() < n {
            return Err(BadFrame);
        }
        let (a, b) = self.0.split_at(n);
        self.0 = b;
        Ok(a)
    }
    pub fn u8(&mut self) -> Result<u8, BadFrame> {
        Ok(self.bytes(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16, BadFrame> {
        let b = self.bytes(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }
    pub fn i16(&mut self) -> Result<i16, BadFrame> {
        Ok(self.u16()? as i16)
    }
    pub fn u32(&mut self) -> Result<u32, BadFrame> {
        let b = self.bytes(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    pub fn u64(&mut self) -> Result<u64, BadFrame> {
        Ok(self.u32()? as u64 | (self.u32()? as u64) << 32)
    }
    pub fn arr<const N: usize>(&mut self) -> Result<[u8; N], BadFrame> {
        let mut a = [0u8; N];
        a.copy_from_slice(self.bytes(N)?);
        Ok(a)
    }
    /// `u8 len` + UTF-8.
    pub fn str(&mut self) -> Result<String, BadFrame> {
        let n = self.u8()? as usize;
        String::from_utf8(self.bytes(n)?.to_vec()).map_err(|_| BadFrame)
    }
    /// `u16 len` + bytes, len <= max.
    pub fn blob(&mut self, max: usize) -> Result<Vec<u8>, BadFrame> {
        let n = self.u16()? as usize;
        if n > max {
            return Err(BadFrame);
        }
        Ok(self.bytes(n)?.to_vec())
    }
    /// The payload must be used up exactly.
    pub fn end(&self) -> Result<(), BadFrame> {
        if self.0.is_empty() {
            Ok(())
        } else {
            Err(BadFrame)
        }
    }
}

/// Frame builder: `Out::new(type)`, fields, `finish()` patches `len`.
pub struct Out(Vec<u8>);

impl Out {
    pub fn new(ty: u8) -> Self {
        let mut v = Vec::with_capacity(64);
        v.extend_from_slice(&[0, 0, 0, 0, ty]);
        Out(v)
    }
    pub fn u8(&mut self, v: u8) -> &mut Self {
        self.0.push(v);
        self
    }
    pub fn u16(&mut self, v: u16) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn i16(&mut self, v: i16) -> &mut Self {
        self.u16(v as u16)
    }
    pub fn u32(&mut self, v: u32) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u64(&mut self, v: u64) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn raw(&mut self, b: &[u8]) -> &mut Self {
        self.0.extend_from_slice(b);
        self
    }
    /// `u8 len` + UTF-8, cut to 255 B on a char boundary.
    pub fn str(&mut self, s: &str) -> &mut Self {
        let mut n = s.len().min(255);
        while !s.is_char_boundary(n) {
            n -= 1;
        }
        self.u8(n as u8).raw(&s.as_bytes()[..n])
    }
    /// `u16 len` + bytes (callers keep b.len() within the field cap).
    pub fn blob(&mut self, b: &[u8]) -> &mut Self {
        self.u16(b.len() as u16).raw(b)
    }
    /// Current frame size, header included.
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn finish(mut self) -> Vec<u8> {
        let len = (self.0.len() - 4) as u32;
        self.0[..4].copy_from_slice(&len.to_le_bytes());
        self.0
    }
}
