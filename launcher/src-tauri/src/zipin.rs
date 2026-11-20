// SPDX-License-Identifier: GPL-3.0-or-later
//! Unpacking a .zip the player chose (a theme pack, themes.rs) into a new folder. A reader of its own, small and strict, because the
//! file is not ours: only stored and deflated entries, no encryption, no zip64, plain relative names inside the folder, a cap on the
//! number of entries and on every size, each entry's size and CRC checked as it is read.

use std::fs;
use std::io::Read;
use std::path::Path;

use flate2::read::DeflateDecoder;

/// What an archive may hold.
pub struct Limits {
    pub max_entries: usize,
    pub max_file: u64,
    pub max_total: u64,
}

fn at16(b: &[u8], o: usize) -> Option<usize> {
    b.get(o..o + 2).map(|s| u16::from_le_bytes([s[0], s[1]]) as usize)
}

fn at32(b: &[u8], o: usize) -> Option<usize> {
    b.get(o..o + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]) as usize)
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
        }
    }
    !crc
}

/// A name inside the archive that may become a file: `/` separated, plain names, nothing that leaves the folder or names a drive.
pub fn entry_name_ok(name: &str) -> bool {
    !name.is_empty()
        && !name.starts_with('/')
        && !name.contains(['\\', ':', '\0'])
        && name.split('/').all(|part| !part.is_empty() && part != "." && part != ".." && !part.chars().any(char::is_control))
}

/// Unpacks `zip` into `out` (a folder that exists and is empty). Directory entries are skipped; folders are made as files need them.
pub fn extract(zip: &[u8], out: &Path, limits: &Limits) -> Result<(), String> {
    let bad = || "This is not a usable .zip file.".to_string();
    let end = (0..=zip.len().saturating_sub(22)).rev().take(65_557).find(|&i| at32(zip, i) == Some(0x0605_4b50)).ok_or_else(bad)?;
    let count = at16(zip, end + 10).ok_or_else(bad)?;
    let mut p = at32(zip, end + 16).ok_or_else(bad)?;
    if count == 0xFFFF || at32(zip, end + 12) == Some(0xFFFF_FFFF) {
        return Err("Zip64 archives are not supported. Make the .zip again with a normal zip program.".into());
    }
    if count > limits.max_entries {
        return Err(format!("The .zip holds {count} entries; at most {} are allowed.", limits.max_entries));
    }
    let mut total = 0u64;
    for _ in 0..count {
        if at32(zip, p) != Some(0x0201_4b50) {
            return Err(bad());
        }
        let (flags, method) = (at16(zip, p + 8).ok_or_else(bad)?, at16(zip, p + 10).ok_or_else(bad)?);
        let (crc, packed, size) = (at32(zip, p + 16).ok_or_else(bad)?, at32(zip, p + 20).ok_or_else(bad)?, at32(zip, p + 24).ok_or_else(bad)?);
        let (n, extra, comment) = (at16(zip, p + 28).ok_or_else(bad)?, at16(zip, p + 30).ok_or_else(bad)?, at16(zip, p + 32).ok_or_else(bad)?);
        let attr = at32(zip, p + 38).ok_or_else(bad)?;
        let local = at32(zip, p + 42).ok_or_else(bad)?;
        let name = std::str::from_utf8(zip.get(p + 46..p + 46 + n).ok_or_else(bad)?).map_err(|_| "A file name in the .zip is not valid text.".to_string())?;
        p += 46 + n + extra + comment;
        if name.ends_with('/') {
            continue;
        }
        if !entry_name_ok(name) {
            return Err(format!("The .zip holds a file with an unsafe name: {name}"));
        }
        if flags & 1 != 0 {
            return Err("The .zip is password protected.".into());
        }
        if (attr >> 16) & 0o170000 == 0o120000 {
            return Err(format!("The .zip holds a link ({name}); links are not allowed."));
        }
        if method != 0 && method != 8 {
            return Err(format!("{name} uses a zip compression this launcher does not read. Make the .zip again with the normal (deflate) setting."));
        }
        if size as u64 > limits.max_file {
            return Err(format!("{name} is larger than {} MB.", limits.max_file / 1_000_000));
        }
        total += size as u64;
        if total > limits.max_total {
            return Err(format!("The .zip unpacks to more than {} MB.", limits.max_total / 1_000_000));
        }
        if at32(zip, local) != Some(0x0403_4b50) {
            return Err(bad());
        }
        let data_at = local + 30 + at16(zip, local + 26).ok_or_else(bad)? + at16(zip, local + 28).ok_or_else(bad)?;
        let raw = zip.get(data_at..data_at + packed).ok_or_else(bad)?;
        let mut data = Vec::with_capacity(size.min(1 << 20));
        if method == 0 {
            data.extend_from_slice(raw);
        } else {
            // never more than the declared size (+1 to notice a lie): a bomb stops here
            DeflateDecoder::new(raw).take(size as u64 + 1).read_to_end(&mut data).map_err(|_| format!("{name} is damaged in the .zip."))?;
        }
        if data.len() != size || crc32(&data) != crc as u32 {
            return Err(format!("{name} is damaged in the .zip."));
        }
        let dest = out.join(name);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
        }
        fs::write(&dest, &data).map_err(|e| format!("cannot write {}: {e}", dest.display()))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::zipout::ZipOut;

    fn tmp(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-zipin-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    const LIM: Limits = Limits { max_entries: 10, max_file: 1000, max_total: 1500 };

    fn zip_of(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut z = ZipOut::new();
        for (n, d) in files {
            z.add(n, d).unwrap();
        }
        z.finish()
    }

    #[test]
    fn plain_names_only() {
        for ok in ["a.png", "dir/a.png", "a b/c.json"] {
            assert!(entry_name_ok(ok), "{ok}");
        }
        for bad in ["", "/a", "../a", "a/../b", "a//b", "a\\b", "C:/a", "a/./b", "a\0b", "./a"] {
            assert!(!entry_name_ok(bad), "{bad}");
        }
    }

    #[test]
    fn extracts_files_and_folders() {
        let out = tmp("ok");
        extract(&zip_of(&[("theme.json", b"{}"), ("art/bg.png", b"png!")]), &out, &LIM).unwrap();
        assert_eq!(fs::read(out.join("theme.json")).unwrap(), b"{}");
        assert_eq!(fs::read(out.join("art/bg.png")).unwrap(), b"png!");
        let _ = fs::remove_dir_all(&out);
    }

    #[test]
    fn a_name_that_leaves_the_folder_is_refused_and_writes_nothing() {
        let out = tmp("slip");
        let err = extract(&zip_of(&[("../evil.txt", b"x")]), &out, &LIM).unwrap_err();
        assert!(err.contains("unsafe name"), "{err}");
        assert!(!out.parent().unwrap().join("evil.txt").exists());
        let _ = fs::remove_dir_all(&out);
    }

    #[test]
    fn caps_hold() {
        let out = tmp("caps");
        assert!(extract(&zip_of(&[("a", &[0u8; 1001])]), &out, &LIM).unwrap_err().contains("larger"));
        assert!(extract(&zip_of(&[("a", &[0u8; 900]), ("b", &[0u8; 900])]), &out, &LIM).unwrap_err().contains("unpacks to more"));
        let many: Vec<(String, Vec<u8>)> = (0..11).map(|i| (format!("f{i}"), vec![1u8])).collect();
        let refs: Vec<(&str, &[u8])> = many.iter().map(|(n, d)| (n.as_str(), d.as_slice())).collect();
        assert!(extract(&zip_of(&refs), &out, &LIM).unwrap_err().contains("entries"));
        let _ = fs::remove_dir_all(&out);
    }

    #[test]
    fn garbage_and_damage_are_refused() {
        let out = tmp("junk");
        assert!(extract(b"not a zip at all, just text padding padding", &out, &LIM).is_err());
        let mut z = zip_of(&[("a.txt", b"hello hello hello")]);
        z[30 + 5 + 1] ^= 0xFF;   // inside the packed bytes (30-byte header, the 5-byte name): the data or its CRC no longer matches
        assert!(extract(&z, &out, &LIM).is_err());
        let _ = fs::remove_dir_all(&out);
    }
}
