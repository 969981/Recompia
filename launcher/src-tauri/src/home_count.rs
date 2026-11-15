// SPDX-License-Identifier: GPL-3.0-or-later
//! "Pokemon at home": how many villager slots of the save are occupied and have a home assigned, read from the committed
//! `G00/CMNDAT.BIN` of the active profile (online.rs sends it when the player switched online presence on).
//!
//! Layout of the 2.0.1 common save (berichan/PokoHex, GPL-3.0: SaveLayout.cs, SaveFile.cs, Villager.cs, VillagerHome.cs; the envelope is
//! the same one runtime/games/pkp/core/pkp_save_uuid.cpp reads): u32 magic at 0, u32 layout at 4, u32 end of the compressed data at 0x10,
//! a zlib stream from 0x130 to that end, inflating to exactly RAW_SIZE bytes. Villager slot i is VILLAGER bytes at i * VILLAGER; it is
//! occupied when any of its bytes +0x01..+0x11 is not zero. Its home record is HOME bytes at HOME_TABLE + i * HOME; byte +0 is the home
//! type, zero meaning no home assigned. Any other layout, or a file that does not check out, gives None (unavailable), never a guess: the
//! occupied count alone is a different number (68 instead of 64 in the save the layout was checked on).

use flate2::read::ZlibDecoder;
use std::io::Read;
use std::path::Path;
use std::time::Duration;

const FILE: &str = "G00/CMNDAT.BIN";
const MAGIC: u32 = 0x3122_7687;
const LAYOUT_2_0_1: u32 = 0x0200_017D;
const END_AT: usize = 0x10;
const HEADER: usize = 0x130;
const RAW_SIZE: usize = 0x47_0CED;
const SLOTS: usize = 1050;
const VILLAGER: usize = 0x176;
const HOME_TABLE: usize = 0x7_9824;
const HOME: usize = 0x15;

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

/// The count from the bytes of a CMNDAT.BIN, or None when it is not the 2.0.1 layout or does not inflate to exactly its size.
pub fn count(file: &[u8]) -> Option<u32> {
    if u32_at(file, 0)? != MAGIC || u32_at(file, 4)? != LAYOUT_2_0_1 {
        return None;
    }
    let end = u32_at(file, END_AT)? as usize;
    let mut raw = Vec::with_capacity(RAW_SIZE);
    // one byte past the size is read on purpose: a stream that inflates to more than RAW_SIZE is refused, and nothing bigger is held
    ZlibDecoder::new(file.get(HEADER..end)?).take(RAW_SIZE as u64 + 1).read_to_end(&mut raw).ok()?;
    if raw.len() != RAW_SIZE {
        return None;
    }
    let at_home = (0..SLOTS)
        .filter(|i| raw[i * VILLAGER + 1..i * VILLAGER + 0x11].iter().any(|&b| b != 0) && raw[HOME_TABLE + i * HOME] != 0)
        .count();
    Some(at_home as u32)
}

/// The count of the active profile's committed save in this game folder; None when there is no save, it is being written, or it is not
/// a layout this knows.
pub fn home_count(game_dir: &Path) -> Option<u32> {
    count(&crate::savebackup::read_committed(game_dir, FILE, Duration::from_secs(1))?)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    /// A CMNDAT.BIN around `raw`: the 2.0.1 header, the zlib stream at HEADER, its end at END_AT.
    fn file_of(raw: &[u8], layout: u32) -> Vec<u8> {
        let mut z = ZlibEncoder::new(Vec::new(), Compression::fast());
        z.write_all(raw).unwrap();
        let stream = z.finish().unwrap();
        let mut f = vec![0u8; HEADER];
        f[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        f[4..8].copy_from_slice(&layout.to_le_bytes());
        f[END_AT..END_AT + 4].copy_from_slice(&((HEADER + stream.len()) as u32).to_le_bytes());
        f.extend_from_slice(&stream);
        f
    }

    /// Slot 0 occupied with a home, slot 1 occupied without one, slot 2 empty but with a stray home type, the last slot occupied with a
    /// home (bounds), every other slot empty: 2 at home.
    fn raw() -> Vec<u8> {
        let mut r = vec![0u8; RAW_SIZE];
        let occupy = |r: &mut Vec<u8>, i: usize, byte: usize| r[i * VILLAGER + byte] = 1;
        occupy(&mut r, 0, 0x01);
        r[HOME_TABLE] = 3;
        occupy(&mut r, 1, 0x10);
        r[HOME_TABLE + 2 * HOME] = 1;
        r[2 * VILLAGER] = 9;   // byte +0 is not part of the occupancy test
        occupy(&mut r, SLOTS - 1, 0x08);
        r[HOME_TABLE + (SLOTS - 1) * HOME] = 1;
        r
    }

    #[test]
    fn counts_occupied_slots_with_a_home_only() {
        assert_eq!(count(&file_of(&raw(), LAYOUT_2_0_1)), Some(2));
    }

    #[test]
    fn an_empty_save_counts_zero() {
        assert_eq!(count(&file_of(&vec![0u8; RAW_SIZE], LAYOUT_2_0_1)), Some(0));
    }

    #[test]
    fn another_magic_or_layout_is_unavailable() {
        let mut f = file_of(&raw(), LAYOUT_2_0_1);
        f[0] ^= 1;
        assert_eq!(count(&f), None);
        assert_eq!(count(&file_of(&raw(), 0x0200_0046)), None, "the 1.0.4 layout has no home table");
    }

    #[test]
    fn a_short_long_or_cut_save_is_unavailable() {
        assert_eq!(count(&file_of(&vec![0u8; RAW_SIZE - 1], LAYOUT_2_0_1)), None);
        assert_eq!(count(&file_of(&vec![0u8; RAW_SIZE + 1], LAYOUT_2_0_1)), None);
        let f = file_of(&raw(), LAYOUT_2_0_1);
        assert_eq!(count(&f[..f.len() - 8]), None, "the end offset points past the file");
        assert_eq!(count(&f[..0x20]), None);
        assert_eq!(count(&[]), None);
    }

    #[test]
    fn the_count_is_read_from_the_committed_save() {
        let d = std::env::temp_dir().join(format!("pkp-home-count-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        // the uid maps to this account folder (saveimport::uid_key), as in savebackup's tests
        let key = "010000000000000070636552484e4341";
        let g = d.join("save").join("account").join(key).join("G00");
        std::fs::create_dir_all(&g).unwrap();
        std::fs::write(d.join(crate::settings::PKP_INI), "[account]\nuid = 000000000000000141434e4852656370\n").unwrap();
        assert_eq!(home_count(&d), None, "no CMNDAT yet");
        std::fs::write(g.join("CMNDAT.BIN"), file_of(&raw(), LAYOUT_2_0_1)).unwrap();
        assert_eq!(home_count(&d), Some(2));
        let _ = std::fs::remove_dir_all(&d);
    }
}
