// SPDX-License-Identifier: GPL-3.0-or-later
//! A tiny zip writer: deflated entries built in memory, for the small archives the launcher hands to the player (logbundle.rs).
//! No zip64, no encryption, no directories: every entry is a file with a plain name. An entry over 4 GiB is refused.

use flate2::write::DeflateEncoder;
use flate2::{Compression, Crc};
use std::io::Write;

struct Entry {
    name: String,
    crc: u32,
    packed: u32,
    size: u32,
    offset: u32,
}

#[derive(Default)]
pub struct ZipOut {
    buf: Vec<u8>,
    entries: Vec<Entry>,
}

/// 1980-01-01 00:00: the archive carries no times, so two bundles of the same files are the same bytes.
const DOS_TIME: u16 = 0;
const DOS_DATE: u16 = (1 << 5) | 1;
/// General purpose bit 11: the names are UTF-8.
const UTF8: u16 = 0x0800;

fn u16le(b: &mut Vec<u8>, v: u16) {
    b.extend_from_slice(&v.to_le_bytes());
}
fn u32le(b: &mut Vec<u8>, v: u32) {
    b.extend_from_slice(&v.to_le_bytes());
}

impl ZipOut {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn add(&mut self, name: &str, data: &[u8]) -> Result<(), String> {
        let size = u32::try_from(data.len()).map_err(|_| format!("{} is too big for a zip.", name))?;
        let mut enc = DeflateEncoder::new(Vec::new(), Compression::default());
        enc.write_all(data).map_err(|e| e.to_string())?;
        let packed = enc.finish().map_err(|e| e.to_string())?;
        let mut crc = Crc::new();
        crc.update(data);
        let offset = u32::try_from(self.buf.len()).map_err(|_| "The zip is too big.".to_string())?;
        let b = &mut self.buf;
        u32le(b, 0x0403_4b50);
        u16le(b, 20);
        u16le(b, UTF8);
        u16le(b, 8);
        u16le(b, DOS_TIME);
        u16le(b, DOS_DATE);
        u32le(b, crc.sum());
        u32le(b, packed.len() as u32);
        u32le(b, size);
        u16le(b, name.len() as u16);
        u16le(b, 0);
        b.extend_from_slice(name.as_bytes());
        b.extend_from_slice(&packed);
        self.entries.push(Entry { name: name.to_string(), crc: crc.sum(), packed: packed.len() as u32, size, offset });
        Ok(())
    }

    /// The finished archive: the entries, then the central directory.
    pub fn finish(mut self) -> Vec<u8> {
        let start = self.buf.len() as u32;
        let b = &mut self.buf;
        for e in &self.entries {
            u32le(b, 0x0201_4b50);
            u16le(b, 20);
            u16le(b, 20);
            u16le(b, UTF8);
            u16le(b, 8);
            u16le(b, DOS_TIME);
            u16le(b, DOS_DATE);
            u32le(b, e.crc);
            u32le(b, e.packed);
            u32le(b, e.size);
            u16le(b, e.name.len() as u16);
            u16le(b, 0);
            u16le(b, 0);
            u16le(b, 0);
            u16le(b, 0);
            u32le(b, 0);
            u32le(b, e.offset);
            b.extend_from_slice(e.name.as_bytes());
        }
        let dir = b.len() as u32 - start;
        u32le(b, 0x0605_4b50);
        u16le(b, 0);
        u16le(b, 0);
        u16le(b, self.entries.len() as u16);
        u16le(b, self.entries.len() as u16);
        u32le(b, dir);
        u32le(b, start);
        u16le(b, 0);
        self.buf
    }
}

/// Reads an archive made by `ZipOut` back: (name, contents) of each entry, every CRC checked.
#[cfg(test)]
pub fn read_all(zip: &[u8]) -> Vec<(String, Vec<u8>)> {
    use flate2::read::DeflateDecoder;
    use std::io::Read;
    let at16 = |o: usize| u16::from_le_bytes([zip[o], zip[o + 1]]) as usize;
    let at32 = |o: usize| u32::from_le_bytes([zip[o], zip[o + 1], zip[o + 2], zip[o + 3]]) as usize;
    let end = zip.len() - 22;
    assert_eq!(at32(end), 0x0605_4b50);
    let (count, mut p) = (at16(end + 10), at32(end + 16));
    let mut out = Vec::new();
    for _ in 0..count {
        assert_eq!(at32(p), 0x0201_4b50);
        let (crc, packed, size, n, local) = (at32(p + 16), at32(p + 20), at32(p + 24), at16(p + 28), at32(p + 42));
        let name = String::from_utf8(zip[p + 46..p + 46 + n].to_vec()).unwrap();
        assert_eq!(at32(local), 0x0403_4b50);
        let data_at = local + 30 + at16(local + 26) + at16(local + 28);
        let mut data = Vec::new();
        DeflateDecoder::new(&zip[data_at..data_at + packed]).read_to_end(&mut data).unwrap();
        assert_eq!(data.len(), size);
        let mut c = Crc::new();
        c.update(&data);
        assert_eq!(c.sum() as usize, crc);
        out.push((name, data));
        p += 46 + n;
    }
    out
}
