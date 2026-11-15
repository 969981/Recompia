// SPDX-License-Identifier: GPL-3.0-or-later
//! The runtime API pkp.exe, pkp_rt.dll and the build tools were made for, read from the files, and whether the three agree.
//!
//! `PKP_RT_API` (game-build/src/pkp_rt_api.h) numbers the call between pkp.exe and pkp_rt.dll; the dll refuses an exe built for
//! another number ("come from different releases"). Since API 2 both files export the number as the data symbol `pkp_rt_api`,
//! read here from the PE export table or ELF dynamic symbol table without loading either file; a file without the export is from an earlier release (None).
//! The tools' number is the define in tools/build/sdk/pkp_rt_api.h, which the next link bakes into pkp.exe.
//! gamebuild.rs's stamp says whether pkp.exe was built with these tools; this says whether it runs with this dll, which the stamp
//! cannot see when an update put only some files in place (a new pkp_rt.dll next to old tools: Set up relinked nothing and the
//! game refused to start). No crate-local import, so examples/first_run.rs builds it next to gamebuild.rs.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// The exported symbol (pkp_stub.cpp, pkp_main.cpp) and the define (pkp_rt_api.h).
const SYMBOL: &[u8] = b"pkp_rt_api\0";
const DEFINE: &str = "#define PKP_RT_API";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Agreement {
    /// one release, or files from before the export (nothing to compare)
    Agree,
    /// pkp.exe was built for another API than pkp_rt.dll, and a link with these tools gives the dll's: Set up relinks it
    ExeStale,
    /// pkp_rt.dll and the tools are from different releases: a link with these tools gives the same refusal again
    Mixed { dll: Option<u32>, sdk: Option<u32> },
}

/// The verdict from the three numbers (None: no export, or no file).
pub fn agreement(exe: Option<u32>, dll: Option<u32>, sdk: Option<u32>) -> Agreement {
    if dll != sdk {
        Agreement::Mixed { dll, sdk }
    } else if exe != dll {
        Agreement::ExeStale
    } else {
        Agreement::Agree
    }
}

/// pkp.exe in the game folder, the pkp_rt.dll Play gives it (the launcher's copy, which host.rs sync_runtime_files puts next to
/// the exe before the start; the game folder's own when the launcher has none) and the tools' header.
pub fn check(game_dir: &Path, tools_dir: &Path) -> Agreement {
    let launcher_dll = crate::host::runtime_lib(tools_dir.parent().unwrap_or(tools_dir));
    let dll = if launcher_dll.is_file() { launcher_dll } else { crate::host::runtime_lib(game_dir) };
    let header = tools_dir.join("build").join("sdk").join("pkp_rt_api.h");
    agreement(export_of(&crate::host::game_exe(game_dir)), export_of(&dll), header_api(&header))
}

/// The number in `#define PKP_RT_API 2u` of a copy of pkp_rt_api.h; None without the file or the line.
pub fn header_api(header: &Path) -> Option<u32> {
    let text = std::fs::read_to_string(header).ok()?;
    let rest = text.lines().map(str::trim).find_map(|l| l.strip_prefix(DEFINE))?;
    let digits: String = rest.trim_start().chars().take_while(char::is_ascii_digit).collect();
    digits.parse().ok()
}

/// The value of the exported `pkp_rt_api` in PE or ELF64 little-endian files; None without a readable export.
pub fn export_of(file: &Path) -> Option<u32> {
    let mut magic = [0u8; 4];
    File::open(file).ok()?.read_exact(&mut magic).ok()?;
    if magic == *b"\x7fELF" {
        Elf::open(file)?.export(SYMBOL)
    } else {
        Pe::open(file)?.export(SYMBOL)
    }
}

#[derive(Clone, Copy)]
struct ElfSection {
    kind: u32,
    addr: u64,
    offset: u64,
    size: u64,
    link: u32,
    entry_size: u64,
}

/// ELF64 little-endian: locate a defined dynamic data symbol and read its file-backed value, without loading code.
struct Elf {
    f: File,
    len: u64,
    sections: Vec<ElfSection>,
}

impl Elf {
    fn open(path: &Path) -> Option<Self> {
        let f = File::open(path).ok()?;
        let mut elf = Self { len: f.metadata().ok()?.len(), f, sections: Vec::new() };
        let h = elf.bytes::<64>(0)?;
        if &h[..6] != b"\x7fELF\x02\x01" || h[6] != 1 {
            return None;
        }
        let shoff = u64::from_le_bytes(h[40..48].try_into().ok()?);
        let stride = u16::from_le_bytes(h[58..60].try_into().ok()?) as u64;
        let count = u16::from_le_bytes(h[60..62].try_into().ok()?) as u64;
        if stride < 64 || count == 0 || count > 4096 || shoff.checked_add(count * stride)? > elf.len {
            return None;
        }
        for i in 0..count {
            let s = elf.bytes::<64>(shoff + i * stride)?;
            elf.sections.push(ElfSection {
                kind: u32::from_le_bytes(s[4..8].try_into().ok()?),
                addr: u64::from_le_bytes(s[16..24].try_into().ok()?),
                offset: u64::from_le_bytes(s[24..32].try_into().ok()?),
                size: u64::from_le_bytes(s[32..40].try_into().ok()?),
                link: u32::from_le_bytes(s[40..44].try_into().ok()?),
                entry_size: u64::from_le_bytes(s[56..64].try_into().ok()?),
            });
        }
        Some(elf)
    }

    fn bytes<const N: usize>(&mut self, off: u64) -> Option<[u8; N]> {
        if off.checked_add(N as u64)? > self.len {
            return None;
        }
        let mut b = [0; N];
        self.f.seek(SeekFrom::Start(off)).ok()?;
        self.f.read_exact(&mut b).ok()?;
        Some(b)
    }

    /// `name` includes its terminator. Only .dynsym exports count, as with the PE export table.
    fn export(&mut self, name: &[u8]) -> Option<u32> {
        let syms = *self.sections.iter().find(|s| s.kind == 11)?; // SHT_DYNSYM
        let strings = *self.sections.get(syms.link as usize)?;
        if syms.entry_size != 24 || syms.size % 24 != 0 || syms.size / 24 > 1 << 20
            || strings.kind != 3 // SHT_STRTAB
            || syms.offset.checked_add(syms.size)? > self.len
            || strings.offset.checked_add(strings.size)? > self.len
        {
            return None;
        }
        for i in 0..syms.size / 24 {
            let s = self.bytes::<24>(syms.offset + i * 24)?;
            let name_off = u32::from_le_bytes(s[..4].try_into().ok()?) as u64;
            let section = u16::from_le_bytes(s[6..8].try_into().ok()?);
            // Defined global/weak object, with default/protected visibility.
            if section == 0 || s[4] & 15 != 1 || !matches!(s[4] >> 4, 1 | 2)
                || !matches!(s[5] & 3, 0 | 3) || name_off.checked_add(name.len() as u64)? > strings.size
            {
                continue;
            }
            let mut found = true;
            for (j, &byte) in name.iter().enumerate() {
                if self.bytes::<1>(strings.offset + name_off + j as u64)?[0] != byte {
                    found = false;
                    break;
                }
            }
            if !found {
                continue;
            }
            let value = u64::from_le_bytes(s[8..16].try_into().ok()?);
            let size = u64::from_le_bytes(s[16..24].try_into().ok()?);
            let data = *self.sections.get(section as usize)?;
            let delta = value.checked_sub(data.addr)?;
            if data.kind == 8 || size < 4 || delta.checked_add(4)? > data.size { // SHT_NOBITS has no file data
                return None;
            }
            return Some(u32::from_le_bytes(self.bytes::<4>(data.offset.checked_add(delta)?)?));
        }
        None
    }
}

/// Just enough of the PE format for one exported value: the headers, the section table (virtual address to file offset) and
/// the export directory. Every read is bounded by the file itself; anything odd gives None.
struct Pe {
    f: File,
    /// (virtual address, raw size, raw pointer) of every section
    sections: Vec<(u32, u32, u32)>,
    /// virtual address of the export directory, 0 for none
    exports: u32,
}

impl Pe {
    fn open(path: &Path) -> Option<Pe> {
        let mut pe = Pe { f: File::open(path).ok()?, sections: Vec::new(), exports: 0 };
        if pe.u16(0)? != 0x5A4D {
            return None; // "MZ"
        }
        let nt = pe.u32(0x3C)? as u64;
        if pe.u32(nt)? != 0x4550 {
            return None; // "PE\0\0"
        }
        let sections = pe.u16(nt + 6)? as u64;
        let opt_size = pe.u16(nt + 20)? as u64;
        let opt = nt + 24;
        let dirs = match pe.u16(opt)? {
            0x20B => opt + 112, // PE32+
            0x10B => opt + 96,  // PE32
            _ => return None,
        };
        if pe.u32(dirs - 4)? < 1 {
            return None; // NumberOfRvaAndSizes: no export entry
        }
        pe.exports = pe.u32(dirs)?;
        for i in 0..sections.min(96) {
            let s = opt + opt_size + i * 40;
            let section = (pe.u32(s + 12)?, pe.u32(s + 16)?, pe.u32(s + 20)?);
            pe.sections.push(section);
        }
        Some(pe)
    }

    fn bytes(&mut self, off: u64, n: usize) -> Option<Vec<u8>> {
        let mut b = vec![0u8; n];
        self.f.seek(SeekFrom::Start(off)).ok()?;
        self.f.read_exact(&mut b).ok()?;
        Some(b)
    }

    fn u16(&mut self, off: u64) -> Option<u16> {
        self.bytes(off, 2).map(|b| u16::from_le_bytes([b[0], b[1]]))
    }

    fn u32(&mut self, off: u64) -> Option<u32> {
        self.bytes(off, 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    /// The file offset of a virtual address: inside a section's raw data (what lies past it is zero in memory, not in the file).
    fn offset(&self, rva: u32) -> Option<u64> {
        self.sections.iter().find(|&&(va, raw, _)| rva >= va && rva - va < raw).map(|&(va, _, ptr)| (rva - va) as u64 + ptr as u64)
    }

    fn u32_at(&mut self, rva: u32) -> Option<u32> {
        let off = self.offset(rva)?;
        self.u32(off)
    }

    /// The u32 an exported data symbol points at, by name.
    fn export(&mut self, name: &[u8]) -> Option<u32> {
        if self.exports == 0 {
            return None;
        }
        let dir = self.offset(self.exports)?;
        let functions = self.u32(dir + 20)?;
        let names = self.u32(dir + 24)?.min(1 << 16);
        let (funcs_rva, names_rva, ords_rva) = (self.u32(dir + 28)?, self.u32(dir + 32)?, self.u32(dir + 36)?);
        for i in 0..names {
            let name_rva = self.u32_at(names_rva.checked_add(4 * i)?)?;
            let at = self.offset(name_rva)?;
            if self.bytes(at, name.len()).as_deref() != Some(name) {
                continue;
            }
            let ord_at = self.offset(ords_rva.checked_add(2 * i)?)?;
            let ord = self.u16(ord_at)? as u32;
            if ord >= functions {
                return None;
            }
            let value_rva = self.u32_at(funcs_rva.checked_add(4 * ord)?)?;
            return self.u32_at(value_rva);
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn temp(name: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("pkp-runtime-api-{}-{}", std::process::id(), name));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    /// A PE32+ image of one section holding an export directory with one data export `symbol` whose value is `value`.
    fn image(symbol: &[u8], value: u32) -> Vec<u8> {
        let put32 = |v: &mut Vec<u8>, off: usize, x: u32| v[off..off + 4].copy_from_slice(&x.to_le_bytes());
        let put16 = |v: &mut Vec<u8>, off: usize, x: u16| v[off..off + 2].copy_from_slice(&x.to_le_bytes());
        let mut v = vec![0u8; 0x400];
        v[..2].copy_from_slice(b"MZ");
        put32(&mut v, 0x3C, 0x40);
        let nt = 0x40;
        v[nt..nt + 4].copy_from_slice(b"PE\0\0");
        put16(&mut v, nt + 4, 0x8664);
        put16(&mut v, nt + 6, 1); // one section
        put16(&mut v, nt + 20, 240); // optional header size
        let opt = nt + 24;
        put16(&mut v, opt, 0x20B);
        put32(&mut v, opt + 108, 16); // directories
        let (va, raw) = (0x1000u32, 0x200usize);
        put32(&mut v, opt + 112, va); // export directory
        put32(&mut v, opt + 116, 40);
        let s = opt + 240;
        v[s..s + 6].copy_from_slice(b".rdata");
        put32(&mut v, s + 8, 0x200);
        put32(&mut v, s + 12, va);
        put32(&mut v, s + 16, 0x200);
        put32(&mut v, s + 20, raw as u32);
        // the section: the directory, the address table, the name pointers, the ordinals, the name, the value
        put32(&mut v, raw + 20, 1);
        put32(&mut v, raw + 24, 1);
        put32(&mut v, raw + 28, va + 40);
        put32(&mut v, raw + 32, va + 44);
        put32(&mut v, raw + 36, va + 48);
        put32(&mut v, raw + 40, va + 96);
        put32(&mut v, raw + 44, va + 52);
        put16(&mut v, raw + 48, 0);
        v[raw + 52..raw + 52 + symbol.len()].copy_from_slice(symbol);
        put32(&mut v, raw + 96, value);
        v
    }

    #[test]
    fn the_exported_number_is_read_from_the_file() {
        let d = temp("export");
        let lib = crate::host::runtime_lib(&d);
        fs::write(&lib, image(SYMBOL, 2)).unwrap();
        assert_eq!(export_of(&lib), Some(2));
        fs::write(d.join("old.dll"), image(b"other_data\0", 2)).unwrap();
        assert_eq!(export_of(&d.join("old.dll")), None, "another export is not ours");
        fs::write(d.join("text.dll"), b"MZ but not a PE file").unwrap();
        assert_eq!(export_of(&d.join("text.dll")), None);
        assert_eq!(export_of(&d.join("missing.dll")), None);
        fs::remove_dir_all(d).unwrap();
    }

    /// Minimal ELF64 with null, .dynsym, .dynstr and .rodata sections; no static symbol table is needed.
    fn elf_image(value: u32, shared: bool) -> Vec<u8> {
        let mut v = vec![0u8; 0x300];
        let put16 = |v: &mut Vec<u8>, off: usize, x: u16| v[off..off + 2].copy_from_slice(&x.to_le_bytes());
        let put32 = |v: &mut Vec<u8>, off: usize, x: u32| v[off..off + 4].copy_from_slice(&x.to_le_bytes());
        let put64 = |v: &mut Vec<u8>, off: usize, x: u64| v[off..off + 8].copy_from_slice(&x.to_le_bytes());
        v[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
        put16(&mut v, 16, if shared { 3 } else { 2 });
        put64(&mut v, 40, 0x40);
        put16(&mut v, 58, 64);
        put16(&mut v, 60, 4);
        for (index, kind, addr, offset, size) in [(1, 11, 0, 0x180, 48), (2, 3, 0, 0x1c0, 12), (3, 1, 0x4000, 0x200, 4)] {
            let s = 0x40 + index * 64;
            put32(&mut v, s + 4, kind);
            put64(&mut v, s + 16, addr);
            put64(&mut v, s + 24, offset);
            put64(&mut v, s + 32, size);
        }
        put32(&mut v, 0x80 + 40, 2);
        put64(&mut v, 0x80 + 56, 24);
        put32(&mut v, 0x198, 1);
        v[0x19c] = 0x11; // STB_GLOBAL | STT_OBJECT
        put16(&mut v, 0x19e, 3);
        put64(&mut v, 0x1a0, 0x4000);
        put64(&mut v, 0x1a8, 4);
        v[0x1c1..0x1c1 + SYMBOL.len()].copy_from_slice(SYMBOL);
        put32(&mut v, 0x200, value);
        v
    }

    #[test]
    fn elf_files_agree_and_still_detect_mismatched_releases() {
        let root = temp("elf-check");
        let tools = root.join("tools");
        let sdk = tools.join("build/sdk");
        fs::create_dir_all(&sdk).unwrap();
        fs::write(sdk.join("pkp_rt_api.h"), "#define PKP_RT_API 2u\n").unwrap();
        let exe = crate::host::game_exe(&root);
        let lib = crate::host::runtime_lib(&root);
        fs::write(&exe, elf_image(2, false)).unwrap();
        fs::write(&lib, elf_image(2, true)).unwrap();
        assert_eq!(export_of(&exe), Some(2));
        assert_eq!(export_of(&lib), Some(2));
        assert_eq!(check(&root, &tools), Agreement::Agree);
        fs::write(&exe, elf_image(1, true)).unwrap(); // PIE uses ET_DYN too
        assert_eq!(check(&root, &tools), Agreement::ExeStale);
        fs::write(&lib, elf_image(3, true)).unwrap();
        assert_eq!(check(&root, &tools), Agreement::Mixed { dll: Some(3), sdk: Some(2) });
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn elf_rejects_missing_exports_and_malformed_tables() {
        let root = temp("elf-invalid");
        let path = root.join("image");
        let valid = elf_image(2, true);
        for (offset, replacement) in [
            (4, 1), (5, 2), // unsupported class / byte order
            (0x1c1, b'x'), (0x1cb, b'x'), // other name / missing terminator
            (0x19e, 0), (0x19c, 1), (0x19d, 2), // undefined / local / hidden
            (0x100 + 4, 8), // NOBITS
            (0x80 + 40, 99), (0x80 + 56, 0), // bad string-table link / entry size
            (0x1a8, 1), (0x100 + 32, 1), // short symbol / section
        ] {
            let mut bytes = valid.clone();
            bytes[offset] = replacement;
            fs::write(&path, bytes).unwrap();
            assert_eq!(export_of(&path), None, "mutation at {offset:#x}");
        }
        for offset in [40, 0x80 + 24, 0xc0 + 24, 0x100 + 24, 0x1a0] {
            let mut bytes = valid.clone();
            bytes[offset..offset + 8].copy_from_slice(&u64::MAX.to_le_bytes());
            fs::write(&path, bytes).unwrap();
            assert_eq!(export_of(&path), None, "overflow at {offset:#x}");
        }
        for len in [4, 63, 0x13f, 0x201] {
            fs::write(&path, &valid[..len]).unwrap();
            assert_eq!(export_of(&path), None, "truncated at {len:#x}");
        }
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn the_define_is_read_from_the_header() {
        let d = temp("header");
        fs::write(d.join("pkp_rt_api.h"), "#pragma once\n#define PKP_RT_API 2u     // bump when this struct changes\n").unwrap();
        assert_eq!(header_api(&d.join("pkp_rt_api.h")), Some(2));
        fs::write(d.join("other.h"), "#define PKP_RT_APIX 3u\n").unwrap();
        assert_eq!(header_api(&d.join("other.h")), None);
        assert_eq!(header_api(&d.join("missing.h")), None);
        fs::remove_dir_all(d).unwrap();
    }

    #[test]
    fn the_three_files_agree_or_not() {
        assert_eq!(agreement(Some(2), Some(2), Some(2)), Agreement::Agree);
        assert_eq!(agreement(None, None, None), Agreement::Agree, "releases before the export: nothing to compare");
        assert_eq!(agreement(None, Some(2), Some(2)), Agreement::ExeStale, "an older exe with this release's dll and tools");
        assert_eq!(agreement(Some(2), Some(3), Some(3)), Agreement::ExeStale);
        assert_eq!(agreement(None, Some(2), None), Agreement::Mixed { dll: Some(2), sdk: None }, "a new dll next to old tools");
        assert_eq!(agreement(Some(2), None, Some(2)), Agreement::Mixed { dll: None, sdk: Some(2) }, "an old dll next to new tools");
    }

    #[test]
    fn check_reads_the_launcher_folder() {
        let root = temp("check");
        let (game, tools) = (root.join("game"), root.join("tools"));
        let sdk = tools.join("build").join("sdk");
        fs::create_dir_all(&game).unwrap();
        fs::create_dir_all(&sdk).unwrap();
        fs::write(sdk.join("pkp_rt_api.h"), "#define PKP_RT_API 2u\n").unwrap();
        fs::write(crate::host::runtime_lib(&root), image(SYMBOL, 2)).unwrap();
        fs::write(crate::host::game_exe(&game), image(b"other_data\0", 0)).unwrap();
        assert_eq!(check(&game, &tools), Agreement::ExeStale, "an exe from before the export");
        fs::write(crate::host::game_exe(&game), image(SYMBOL, 2)).unwrap();
        assert_eq!(check(&game, &tools), Agreement::Agree);
        fs::write(sdk.join("pkp_rt_api.h"), "#define PKP_RT_API 3u\n").unwrap();
        assert_eq!(check(&game, &tools), Agreement::Mixed { dll: Some(2), sdk: Some(3) });
        fs::remove_file(crate::host::runtime_lib(&root)).unwrap();
        fs::write(crate::host::runtime_lib(&game), image(SYMBOL, 3)).unwrap();
        assert_eq!(check(&game, &tools), Agreement::ExeStale, "without a launcher copy the game folder's dll counts");
        fs::remove_dir_all(root).unwrap();
    }
}
