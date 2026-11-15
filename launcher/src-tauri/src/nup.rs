// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads a `.nup` package (https://github.com/a55uka/nup, format 1) for unpack.rs: the header and table of contents
//! (size, SHA-256 of the TOC, every record's bounds) are checked when it is opened, before anything is written. A
//! non-standalone update gets the base's RomFS files underneath its own (the update's files win).
//!
//! The checks (TOC hash, record validation, standalone layering) follow pokopia-native (MIT), nupread.py and nxs2.py
//! nup_unpack @7c3655d.

use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom, Write};

use crate::unpack::{self, Program};

pub const MAGIC: &[u8] = b"NUP\x01";
const LABEL: &str = ".nup";
const NO_DIR: u32 = 0xFFFF_FFFF;
const KIND_BASE: u8 = 0;
const KIND_UPDATE: u8 = 1;
const KIND_ADDON: u8 = 2;
const DAMAGED: &str = "The .nup file is damaged or incomplete.";

struct Package {
    title_id: u64,
    root_dir: u32,
    exefs_dir: u32,
    romfs_dir: u32,
    version: u32,
    kind: u8,
    /// Package flag bit 0: the RomFS is complete on its own. Without it an update only holds the files it changes.
    standalone: bool,
}

struct Dir {
    name: String,
    first_child: u32,
    child_count: u32,
}

struct FileNode {
    name: String,
    size: u64,
    extent_offset: u64,
    extent_size: u64,
    sha: [u8; 32],
    codec: u8,
}

struct Nup {
    uuid: [u8; 16],
    file_size: u64,
    packages: Vec<Package>,
    dirs: Vec<Dir>,
    files: Vec<FileNode>,
    children: Vec<u32>,
}

fn u32_at(b: &[u8], o: usize) -> Result<u32, String> {
    b.get(o..o.saturating_add(4)).map(|s| u32::from_le_bytes(s.try_into().unwrap())).ok_or_else(|| DAMAGED.to_string())
}
fn u64_at(b: &[u8], o: usize) -> Result<u64, String> {
    b.get(o..o.saturating_add(8)).map(|s| u64::from_le_bytes(s.try_into().unwrap())).ok_or_else(|| DAMAGED.to_string())
}

fn parse(f: &mut File) -> Result<Nup, String> {
    let real_size = f.metadata().map_err(|e| e.to_string())?.len();
    let mut sb = [0u8; 0x80];
    f.read_exact(&mut sb).map_err(|_| "This file is too small to be a .nup package.".to_string())?;
    if &sb[0..4] != MAGIC {
        return Err("This is not a .nup package (wrong header).".into());
    }
    let header_size = u16::from_le_bytes([sb[4], sb[5]]) as u64;
    let toc_offset = u64_at(&sb, 0x08)?;
    let toc_size = u64_at(&sb, 0x10)?;
    let toc_hash = &sb[0x18..0x38];
    let blob_offset = u64_at(&sb, 0x38)?;
    let file_size = u64_at(&sb, 0x40)?;
    let pkg_count = u32_at(&sb, 0x48)? as usize;
    if u32_at(&sb, 0x4C)? != 0 {
        return Err("This .nup was made with a newer format than this launcher knows. Update the launcher.".into());
    }
    if header_size < 0x80
        || toc_offset != header_size
        || blob_offset % 16 != 0
        || toc_offset.checked_add(toc_size).map_or(true, |e| e > blob_offset)
        || file_size != real_size
        || toc_size > (1 << 30)
    {
        return Err("The .nup file is damaged or incomplete (it does not match its header). Download it again.".into());
    }
    if !(1..=4096).contains(&pkg_count) {
        return Err("The .nup package count is not valid.".into());
    }
    let mut uuid = [0u8; 16];
    uuid.copy_from_slice(&sb[0x50..0x60]);

    let mut toc = vec![0u8; toc_size as usize];
    f.seek(SeekFrom::Start(toc_offset)).map_err(|e| e.to_string())?;
    f.read_exact(&mut toc).map_err(|_| DAMAGED.to_string())?;
    if Sha256::digest(&toc).as_slice() != toc_hash {
        return Err("The .nup table of contents is damaged (checksum mismatch). Download the file again.".into());
    }
    if toc.get(0..4) != Some(b"TOC\x01".as_slice()) {
        return Err("The .nup table of contents is invalid.".into());
    }
    let toc_pkgs = u32_at(&toc, 4)? as usize;
    let dir_count = u64_at(&toc, 8)?;
    let file_count = u64_at(&toc, 0x10)?;
    let pool_off = u64_at(&toc, 0x18)?;
    let pool_size = u64_at(&toc, 0x20)?;
    let toc_len = toc.len() as u64;
    // counts come from the file: bound them by the TOC size before allocating anything
    if toc_pkgs != pkg_count || dir_count > toc_len / 0x18 || file_count > toc_len / 0x50 {
        return Err("The .nup table of contents is inconsistent.".into());
    }
    let (dir_count, file_count) = (dir_count as usize, file_count as usize);
    let pool_end = pool_off.checked_add(pool_size).filter(|e| *e <= toc_len).ok_or("The .nup string table is out of bounds.")?;
    let pool = &toc[pool_off as usize..pool_end as usize];
    let name_at = |off: u32, len: u32| -> Result<String, String> {
        let (o, l) = (off as usize, len as usize);
        Ok(String::from_utf8_lossy(pool.get(o..o.saturating_add(l)).ok_or("A name in the .nup is out of bounds.")?).into_owned())
    };

    let pkg_at = 0x30usize;
    let dir_at = pkg_at + 0x40 * pkg_count;
    let file_at = dir_at + 0x18 * dir_count;
    let child_at = file_at + 0x50 * file_count;
    if child_at as u64 > pool_off {
        return Err("The .nup table of contents is inconsistent.".into());
    }
    let mut packages = Vec::with_capacity(pkg_count);
    for i in 0..pkg_count {
        let pos = pkg_at + 0x40 * i;
        let p = Package {
            title_id: u64_at(&toc, pos)?,
            root_dir: u32_at(&toc, pos + 0x08)?,
            exefs_dir: u32_at(&toc, pos + 0x0C)?,
            romfs_dir: u32_at(&toc, pos + 0x10)?,
            version: u32_at(&toc, pos + 0x14)?,
            kind: *toc.get(pos + 0x18).ok_or(DAMAGED)?,
            standalone: toc.get(pos + 0x19).ok_or(DAMAGED)? & 1 != 0,
        };
        if p.kind >= 5 {
            return Err(format!("The .nup holds a package type ({}) this launcher does not know.", p.kind));
        }
        for d in [p.root_dir, p.exefs_dir, p.romfs_dir] {
            if d != NO_DIR && d as usize >= dir_count {
                return Err("The .nup package tree is invalid.".into());
            }
        }
        packages.push(p);
    }
    let mut dirs = Vec::with_capacity(dir_count);
    let mut slots = 0u64;
    for i in 0..dir_count {
        let pos = dir_at + 0x18 * i;
        let d = Dir { name: name_at(u32_at(&toc, pos)?, u32_at(&toc, pos + 4)?)?, first_child: u32_at(&toc, pos + 8)?, child_count: u32_at(&toc, pos + 0xC)? };
        slots += d.child_count as u64;
        dirs.push(d);
    }
    let mut files = Vec::with_capacity(file_count);
    for i in 0..file_count {
        let pos = file_at + 0x50 * i;
        let mut sha = [0u8; 32];
        sha.copy_from_slice(toc.get(pos + 0x20..pos + 0x40).ok_or(DAMAGED)?);
        let f = FileNode {
            name: name_at(u32_at(&toc, pos)?, u32_at(&toc, pos + 4)?)?,
            size: u64_at(&toc, pos + 8)?,
            extent_offset: u64_at(&toc, pos + 0x10)?,
            extent_size: u64_at(&toc, pos + 0x18)?,
            sha,
            codec: *toc.get(pos + 0x40).ok_or(DAMAGED)?,
        };
        if f.codec > 1 {
            return Err(format!("{} uses a compression ({}) this launcher does not know.", f.name, f.codec));
        }
        if f.extent_size > 0 && (f.extent_offset < blob_offset || f.extent_offset.checked_add(f.extent_size).map_or(true, |e| e > real_size) || f.extent_offset % 16 != 0) {
            return Err(format!("{} points outside the .nup file.", f.name));
        }
        if f.codec == 0 && f.extent_size != f.size {
            return Err(format!("{} has the wrong stored size.", f.name));
        }
        files.push(f);
    }
    if child_at as u64 + 4 * slots > pool_off {
        return Err("The .nup folder listing runs past the table of contents.".into());
    }
    let children = (0..slots as usize).map(|i| u32_at(&toc, child_at + i * 4)).collect::<Result<Vec<_>, _>>()?;
    if dirs.iter().any(|d| d.first_child as u64 + d.child_count as u64 > slots) {
        return Err("The .nup folder listing is out of bounds.".into());
    }
    Ok(Nup { uuid, file_size: real_size, packages, dirs, files, children })
}

impl Nup {
    /// Every file below a folder as (relative path with '/', file index), in the container's order.
    fn walk(&self, d: u32, prefix: &str, depth: u32, out: &mut Vec<(String, usize)>) -> Result<(), String> {
        if depth > 64 {
            return Err("The .nup folder tree is too deep.".into());
        }
        let dir = self.dirs.get(d as usize).ok_or("The .nup refers to a folder that does not exist.")?;
        let dc = self.dirs.len() as u32;
        for slot in dir.first_child..dir.first_child + dir.child_count {
            let id = *self.children.get(slot as usize).ok_or("The .nup folder listing is out of bounds.")?;
            if id < dc {
                self.walk(id, &format!("{}{}/", prefix, unpack::checked_name(&self.dirs[id as usize].name, LABEL)?), depth + 1, out)?;
            } else {
                let fi = (id - dc) as usize;
                let f = self.files.get(fi).ok_or("The .nup refers to a file that does not exist.")?;
                out.push((format!("{}{}", prefix, unpack::checked_name(&f.name, LABEL)?), fi));
            }
        }
        Ok(())
    }

    fn tree(&self, d: u32) -> Result<Vec<(String, usize)>, String> {
        let mut v = Vec::new();
        if d != NO_DIR {
            self.walk(d, "", 0, &mut v)?;
        }
        Ok(v)
    }
}

impl Nup {
    fn programs(&self) -> Vec<Program> {
        // add-ons and system packages are not programs
        let prog = |p: &Package| (p.kind == KIND_BASE || p.kind == KIND_UPDATE) && p.exefs_dir != NO_DIR;
        self.packages.iter().enumerate().filter(|(_, p)| prog(p)).map(|(id, p)| Program { update: p.kind == KIND_UPDATE, version: p.version, id }).collect()
    }

    /// The RomFS of a program; a non-standalone update only holds what it changes, so the base's files go under it.
    fn romfs(&self, id: usize) -> Result<BTreeMap<String, usize>, String> {
        let p = &self.packages[id];
        let mut files: BTreeMap<String, usize> = BTreeMap::new();
        if p.romfs_dir != NO_DIR {
            files.extend(self.tree(p.romfs_dir)?);
        }
        if p.kind == KIND_UPDATE && !p.standalone {
            let base = self.packages.iter().find(|q| q.kind == KIND_BASE && q.romfs_dir != NO_DIR);
            match base {
                Some(base) => {
                    for (rel, fi) in self.tree(base.romfs_dir)? {
                        files.entry(rel).or_insert(fi);
                    }
                }
                // A packer that kept an empty `base/` folder still writes a base record, just with nothing in it (empty
                // ExeFS, no RomFS), and such a container's update has to keep standalone_update clear (SPEC 3.1.2). That
                // record is not a base game: an update whose own RomFS holds the whole game needs nothing under it, so ask
                // for the base only when the update's own RomFS is short of what the supported version has.
                None => {
                    let whole = crate::dump::supported_romfs().map_or(false, |need| {
                        let (n, b) = files.values().fold((0u64, 0u64), |(n, b), &fi| (n + 1, b + self.files[fi].size));
                        n >= need.files && b >= need.bytes
                    });
                    if !whole {
                        return Err("This update only holds the files it changes and needs the base game, which is not in this .nup. Use a .nup that contains the base game too.".into());
                    }
                }
            }
        }
        Ok(files)
    }

    /// The RomFS files of every add-on package, by title id.
    fn add_ons(&self) -> Result<Vec<(u64, Vec<(String, usize)>)>, String> {
        self.packages.iter().filter(|p| p.kind == KIND_ADDON && p.romfs_dir != NO_DIR).map(|p| Ok((p.title_id, self.tree(p.romfs_dir)?))).collect()
    }
}

/// A .nup that is open for unpacking.
pub struct Reader {
    nup: Nup,
    src: BufReader<File>,
}

pub fn open(mut file: File) -> Result<Reader, String> {
    let nup = parse(&mut file)?;
    Ok(Reader { nup, src: BufReader::with_capacity(1 << 20, file) })
}

impl unpack::Package for Reader {
    fn label(&self) -> &'static str {
        LABEL
    }
    fn stamp(&self) -> String {
        format!("{}:{}", self.nup.uuid.iter().map(|b| format!("{:02x}", b)).collect::<String>(), self.nup.file_size)
    }
    fn programs(&self) -> Vec<Program> {
        self.nup.programs()
    }
    fn exefs(&self, p: &Program) -> Result<Vec<(String, usize)>, String> {
        self.nup.tree(self.nup.packages[p.id].exefs_dir)
    }
    fn romfs(&self, p: &Program) -> Result<BTreeMap<String, usize>, String> {
        self.nup.romfs(p.id)
    }
    fn add_ons(&self) -> Result<Vec<(u64, Vec<(String, usize)>)>, String> {
        self.nup.add_ons()
    }
    fn size(&self, file: usize) -> u64 {
        self.nup.files[file].size
    }
    fn sha256(&self, file: usize) -> [u8; 32] {
        self.nup.files[file].sha
    }
    fn read(&mut self, file: usize, name: &str, w: &mut dyn Write, cancel: &dyn Fn() -> bool) -> Result<(), String> {
        let f = &self.nup.files[file];
        if f.extent_size == 0 {
            return Ok(());
        }
        self.src.seek(SeekFrom::Start(f.extent_offset)).map_err(|e| e.to_string())?;
        let part = (&mut self.src).take(f.extent_size);
        match f.codec {
            0 => unpack::copy(&mut part.take(f.size), w, cancel, name, LABEL),
            _ => {
                let dec = zstd::stream::read::Decoder::new(part).map_err(|e| format!("{} could not be decompressed: {}", name, e))?;
                unpack::copy(&mut dec.take(f.size + 1), w, cancel, name, LABEL)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_ons_are_the_romfs_of_add_on_packages() {
        let dir = |name: &str, first_child, child_count| Dir { name: name.into(), first_child, child_count };
        let file = FileNode { name: "a.bin".into(), size: 1, extent_offset: 0, extent_size: 0, sha: [0; 32], codec: 0 };
        let pkg = |kind, romfs_dir| Package { title_id: 0x0400A3A01EE4D001, root_dir: 0, exefs_dir: NO_DIR, romfs_dir, version: 0, kind, standalone: false };
        let nup = Nup {
            uuid: [0; 16],
            file_size: 0,
            // the base's romfs is not an add-on; the add-on's is
            packages: vec![pkg(KIND_BASE, 0), pkg(KIND_ADDON, 0), pkg(KIND_ADDON, NO_DIR)],
            dirs: vec![dir("romfs", 0, 1), dir("sub", 1, 1)],
            files: vec![file],
            children: vec![1, 2], // ids past the folders are files
        };
        assert_eq!(nup.add_ons().unwrap(), vec![(0x0400A3A01EE4D001, vec![("sub/a.bin".to_string(), 0)])]);
        assert!(nup.programs().is_empty(), "a base without an ExeFS is not a program");
    }
}
