// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads an `.nx` archive (https://github.com/fab918/nxformat, docs/SPEC.md, header versions 0.2 and 0.3) for unpack.rs.
//!
//! Layout: a 256-byte header in each of the first two 4096-byte pages (the valid one with the highest generation
//! counts), the data blocks from 8192, then the Catalog (applications and their snapshots), the Manifest (folders,
//! files, and the blocks each file is made of) and the block index. A snapshot is one whole version of the game: an
//! update already holds the base game's files, so nothing is layered here. A file is a row of blocks of one chunk
//! size, each stored as it is or as one Zstandard frame.
//!
//! Only archives made from extracted game files or from a .nup hold `exefs/` and `romfs/` trees. One made from an
//! .nsp or .xci holds the NCA packages themselves, which the launcher does not read, and is refused in words.
//!
//! The archive is a file the player supplies: the header CRC, the SHA-256 of the Catalog, the Manifest and the block
//! index, and every table range are checked when it is opened, every block's range when it is read. The per-slice
//! CRC-32C table is not used: unpack.rs checks the SHA-256 of every whole file, which covers the same bytes.

use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};

use crate::unpack::{self, Program};

pub const MAGIC: &[u8] = b"NXAR";
const LABEL: &str = ".nx";
const PAGE: usize = 4096;
const HEADER: usize = 256;
const DATA_START: u64 = 2 * PAGE as u64;
const MAX_CATALOG: u64 = 16 << 20;
/// What is held in memory. The format allows more; a game's file list and block index are a few MB.
const MAX_MANIFEST: u64 = 256 << 20;
const MAX_INDEX: u64 = 256 << 20;
const MAX_DEPTH: u32 = 64;
/// A stored block: a 1 MiB chunk plus what a Zstandard frame may add.
const MAX_STORED: u64 = (1 << 20) + (64 << 10);
/// Catalog extensions marked "required" that change nothing about the file trees read here.
const KNOWN_REQUIRED: [u32; 3] = [0x0001_0004, 0x0001_0005, 0x0001_0008];
const TYPE_BASE: u8 = 0;
const TYPE_UPDATE: u8 = 1;
const TYPE_DLC: u8 = 2;
const TYPE_STANDALONE_UPDATE: u8 = 3;
/// Snapshot flag: `content_id` holds the add-on's title id (a DLC that came from a .nup or from extracted files).
const FLAG_ADDON_TITLE_ID: u8 = 2;
const KIND_DIR: u8 = 1;
const KIND_FILE: u8 = 2;
const DAMAGED: &str = "The .nx file is damaged or incomplete (it does not match its header). Download it again.";
const NOT_NX: &str = "This is not an .nx package (wrong header).";
const NEWER: &str = "This .nx was made with a newer format than this launcher knows. Update the launcher.";
const NATIVE: &str = "This .nx holds the game as NCA packages (it was made from an .nsp or .xci), which the launcher cannot read. Make the .nx from extracted game files or from a .nup (nxconvert pack-tree or pack-nup).";

fn damaged() -> String {
    DAMAGED.to_string()
}
fn u16_at(b: &[u8], o: usize) -> Result<u16, String> {
    b.get(o..o.saturating_add(2)).map(|s| u16::from_le_bytes(s.try_into().unwrap())).ok_or_else(damaged)
}
fn u32_at(b: &[u8], o: usize) -> Result<u32, String> {
    b.get(o..o.saturating_add(4)).map(|s| u32::from_le_bytes(s.try_into().unwrap())).ok_or_else(damaged)
}
fn u64_at(b: &[u8], o: usize) -> Result<u64, String> {
    b.get(o..o.saturating_add(8)).map(|s| u64::from_le_bytes(s.try_into().unwrap())).ok_or_else(damaged)
}

/// CRC-32C (Castagnoli, reflected). Only the two headers are checked with it, so one bit at a time is fast enough.
fn crc32c(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0x82F6_3B78 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

/// The header of a page when the page is whole: the magic, the CRC, and nothing after the header.
fn slot(page: &[u8]) -> Option<&[u8]> {
    let h = &page[..HEADER];
    let whole = &h[..4] == MAGIC && crc32c(&h[..252]).to_le_bytes() == h[252..] && page[HEADER..].iter().all(|&b| b == 0);
    whole.then_some(h)
}

/// A table of `count` records of `size` bytes at `off`, as the byte range it takes in `b`.
fn table(b: &[u8], off: u64, count: u64, size: u64) -> Result<(usize, usize), String> {
    let end = count.checked_mul(size).and_then(|n| off.checked_add(n)).filter(|e| *e <= b.len() as u64).ok_or_else(damaged)?;
    Ok((off as usize, (end - off) as usize / size as usize))
}

/// The three checked parts of one generation of the archive.
struct Meta {
    catalog: Vec<u8>,
    manifest: Vec<u8>,
    index: Vec<u8>,
    archive_size: u64,
    manifest_sha: [u8; 32],
}

fn load(f: &mut File, h: &[u8], real_size: u64) -> Result<Meta, String> {
    if u16_at(h, 4)? != 0 || !matches!(u16_at(h, 6)?, 2 | 3) || u16_at(h, 12)? != 1 || u16_at(h, 14)? != 0 {
        return Err(NEWER.into());
    }
    let archive_size = u64_at(h, 16)?;
    if u32_at(h, 8)? != HEADER as u32 || u32_at(h, 224)? != 16384 || h[228..252].iter().any(|&b| b != 0) || archive_size > real_size || archive_size > i64::MAX as u64 {
        return Err(damaged());
    }
    let mut region = |off: u64, size: u64, min: u64, max: u64, what: &str| -> Result<Vec<u8>, String> {
        if size > max {
            return Err(format!("This .nx is too large for the launcher to read (its {} takes {} MB).", what, size >> 20));
        }
        if size < min || off < DATA_START || off % 16 != 0 || off.checked_add(size).map_or(true, |e| e > archive_size) {
            return Err(damaged());
        }
        let mut v = vec![0u8; size as usize];
        f.seek(SeekFrom::Start(off)).and_then(|_| f.read_exact(&mut v)).map_err(|_| damaged())?;
        Ok(v)
    };
    let catalog = region(u64_at(h, 24)?, u64_at(h, 32)?, 128, MAX_CATALOG, "catalog")?;
    let manifest = region(u64_at(h, 72)?, u64_at(h, 80)?, 96, MAX_MANIFEST, "file list")?;
    let blocks = u64_at(h, 96)?;
    let (index, integrity) = if blocks == 0 {
        (Vec::new(), Vec::new())
    } else {
        let size = blocks.checked_mul(24).ok_or_else(damaged)?;
        (region(u64_at(h, 88)?, size, 24, MAX_INDEX, "block index")?, region(u64_at(h, 104)?, u64_at(h, 112)?, 4, MAX_INDEX, "block index")?)
    };
    let index_sha = Sha256::new().chain_update(&index).chain_update(&integrity).finalize();
    if Sha256::digest(&catalog).as_slice() != &h[120..152] || Sha256::digest(&manifest).as_slice() != &h[152..184] || index_sha.as_slice() != &h[184..216] {
        return Err(damaged());
    }
    let mut manifest_sha = [0u8; 32];
    manifest_sha.copy_from_slice(&h[152..184]);
    Ok(Meta { catalog, manifest, index, archive_size, manifest_sha })
}

/// One file record of the Manifest.
struct FileRec {
    size: u64,
    first_ref: u32,
    blocks: u32,
    chunk_log2: u8,
    sha: [u8; 32],
}

/// The Manifest: folders, their entries, files, and the block each chunk of a file is in. The table ranges are checked
/// once, here; a record index is checked where it is taken from the file.
struct Tree {
    m: Vec<u8>,
    dirs: (usize, usize),
    files: (usize, usize),
    entries: (usize, usize),
    refs: (usize, usize),
    pool: (u64, u64),
}

impl Tree {
    fn parse(m: Vec<u8>) -> Result<Tree, String> {
        if m.get(..4) != Some(b"NXMF".as_slice()) || u16_at(&m, 4)? != 1 || u16_at(&m, 6)? != 96 || u64_at(&m, 8)? != m.len() as u64 {
            return Err(damaged());
        }
        let dirs = table(&m, u64_at(&m, 32)?, u32_at(&m, 16)? as u64, 16)?;
        let files = table(&m, u64_at(&m, 40)?, u32_at(&m, 20)? as u64, 64)?;
        let entries = table(&m, u64_at(&m, 48)?, u32_at(&m, 24)? as u64, 24)?;
        let refs = table(&m, u64_at(&m, 56)?, u32_at(&m, 28)? as u64, 4)?;
        let pool = (u64_at(&m, 64)?, u64_at(&m, 72)?);
        table(&m, pool.0, pool.1, 1)?;
        Ok(Tree { m, dirs, files, entries, refs, pool })
    }

    /// The entries of a folder as (name, kind, target).
    fn list(&self, dir: u32) -> Result<Vec<(&str, u8, u32)>, String> {
        if dir as usize >= self.dirs.1 {
            return Err(damaged());
        }
        let rec = self.dirs.0 + 16 * dir as usize;
        let (first, count) = (u32_at(&self.m, rec)? as usize, u32_at(&self.m, rec + 4)? as usize);
        if first.checked_add(count).map_or(true, |e| e > self.entries.1) {
            return Err(damaged());
        }
        let mut out = Vec::with_capacity(count);
        for i in first..first + count {
            let e = self.entries.0 + 24 * i;
            let (off, len) = (u64_at(&self.m, e)?, u32_at(&self.m, e + 8)? as u64);
            if off < self.pool.0 || off - self.pool.0 > self.pool.1 || len > self.pool.1 - (off - self.pool.0) {
                return Err(damaged());
            }
            let name = std::str::from_utf8(&self.m[off as usize..(off + len) as usize]).map_err(|_| damaged())?;
            out.push((unpack::checked_name(name, LABEL)?, self.m[e + 16], u32_at(&self.m, e + 12)?));
        }
        Ok(out)
    }

    /// The folder `name` in `dir`, if there is one.
    fn subdir(&self, dir: u32, name: &str) -> Result<Option<u32>, String> {
        Ok(self.list(dir)?.into_iter().find(|e| e.0 == name && e.1 == KIND_DIR).map(|e| e.2))
    }

    /// A file record whose index a walk has checked.
    fn file(&self, id: usize) -> Result<FileRec, String> {
        let r = self.files.0 + 64 * id;
        let mut sha = [0u8; 32];
        sha.copy_from_slice(self.m.get(r + 20..r + 52).ok_or_else(damaged)?);
        Ok(FileRec { size: u64_at(&self.m, r)?, first_ref: u32_at(&self.m, r + 8)?, blocks: u32_at(&self.m, r + 12)?, chunk_log2: self.m[r + 16], sha })
    }

    /// Every file below a folder as (relative path with '/', file id). No folder or file may be reached twice: that is
    /// how a loop in the folder records shows.
    fn walk(&self, dir: u32, prefix: &str, depth: u32, seen: &mut HashSet<(u8, u32)>, out: &mut Vec<(String, usize)>) -> Result<(), String> {
        if depth > MAX_DEPTH || !seen.insert((KIND_DIR, dir)) {
            return Err(damaged());
        }
        for (name, kind, target) in self.list(dir)? {
            match kind {
                KIND_DIR => self.walk(target, &format!("{}{}/", prefix, name), depth + 1, seen, out)?,
                KIND_FILE if (target as usize) < self.files.1 && seen.insert((KIND_FILE, target)) => {
                    let f = self.file(target as usize)?;
                    // a file is exactly as many blocks as its size takes at its chunk size
                    let whole = (16..=20).contains(&f.chunk_log2) && f.size <= i64::MAX as u64 && f.size.div_ceil(1 << f.chunk_log2) == f.blocks as u64;
                    if !whole || f.first_ref as usize + f.blocks as usize > self.refs.1 {
                        return Err(damaged());
                    }
                    out.push((format!("{}{}", prefix, name), target as usize));
                }
                _ => return Err(damaged()),
            }
        }
        Ok(())
    }

    fn tree(&self, dir: u32) -> Result<Vec<(String, usize)>, String> {
        let mut v = Vec::new();
        self.walk(dir, "", 0, &mut HashSet::new(), &mut v)?;
        Ok(v)
    }
}

/// A base game or update with an ExeFS: the folders of its snapshot.
struct Prog {
    update: bool,
    version: u32,
    exefs: u32,
    romfs: Option<u32>,
}

/// An .nx that is open for unpacking.
pub struct Reader {
    src: File,
    tree: Tree,
    index: Vec<u8>,
    archive_size: u64,
    manifest_sha: [u8; 32],
    progs: Vec<Prog>,
    /// (title id, romfs folder) of every add-on
    add_ons: Vec<(u64, u32)>,
    stored: Vec<u8>,
    plain: Vec<u8>,
    zstd: zstd::bulk::Decompressor<'static>,
}

pub fn open(mut file: File) -> Result<Reader, String> {
    let real_size = file.metadata().map_err(|e| e.to_string())?.len();
    let mut head = vec![0u8; 2 * PAGE];
    if file.read_exact(&mut head).is_err() {
        return Err(if head.starts_with(MAGIC) { damaged() } else { NOT_NX.into() });
    }
    // the valid header with the highest generation; the other one is tried when the newest generation is damaged
    let generation = |h: &[u8]| u64_at(h, 216).unwrap_or(0);
    let order = match (slot(&head[..PAGE]), slot(&head[PAGE..])) {
        (Some(a), Some(b)) if generation(a) == generation(b) => {
            if a != b {
                return Err(damaged());
            }
            vec![a]
        }
        (Some(a), Some(b)) => if generation(a) > generation(b) { vec![a, b] } else { vec![b, a] },
        (Some(h), None) | (None, Some(h)) => vec![h],
        (None, None) => return Err(if head.starts_with(MAGIC) || head[PAGE..].starts_with(MAGIC) { damaged() } else { NOT_NX.into() }),
    };
    let mut meta = Err(damaged());
    for h in order {
        meta = load(&mut file, h, real_size);
        if meta.is_ok() {
            break;
        }
    }
    let Meta { catalog: c, manifest, index, archive_size, manifest_sha } = meta?;
    let tree = Tree::parse(manifest)?;

    if c.get(..4) != Some(b"NXCT".as_slice()) || u16_at(&c, 4)? != 1 || u16_at(&c, 6)? != 128 || u64_at(&c, 8)? != c.len() as u64 {
        return Err(damaged());
    }
    let apps = table(&c, u64_at(&c, 48)?, u32_at(&c, 20)? as u64, 80)?;
    let snaps = table(&c, u64_at(&c, 64)?, u32_at(&c, 28)? as u64, 120)?;
    let exts = table(&c, u64_at(&c, 88)?, u32_at(&c, 40)? as u64, 32)?;
    for i in 0..exts.1 {
        let e = exts.0 + 32 * i;
        if u32_at(&c, e + 4)? & 1 != 0 && !KNOWN_REQUIRED.contains(&u32_at(&c, e)?) {
            return Err(NEWER.into());
        }
    }
    // an archive may hold several games: then only this game's snapshots count
    let app_id = |i: usize| u64_at(&c, apps.0 + 80 * i);
    let mut ours = Vec::new();
    for i in 0..apps.1 {
        if app_id(i)? == crate::saveimport::PROGRAM_ID {
            ours.push(i);
        }
    }
    let picked: Vec<usize> = if apps.1 > 1 && !ours.is_empty() { ours } else { (0..apps.1).collect() };

    let (mut progs, mut add_ons, mut native) = (Vec::<Prog>::new(), Vec::new(), false);
    for app in picked {
        let a = apps.0 + 80 * app;
        let (first, count) = (u32_at(&c, a + 20)? as usize, u32_at(&c, a + 24)? as usize);
        if first.checked_add(count).map_or(true, |e| e > snaps.1) {
            return Err(damaged());
        }
        for s in (first..first + count).map(|i| snaps.0 + 120 * i) {
            let (kind, flags, version, root) = (c[s + 4], c[s + 5], u32_at(&c, s + 8)?, u32_at(&c, s + 12)?);
            let (exefs, romfs) = (tree.subdir(root, "exefs")?, tree.subdir(root, "romfs")?);
            if exefs.is_none() && romfs.is_none() {
                native |= tree.list(root)?.iter().any(|e| e.1 == KIND_FILE && e.0.to_ascii_lowercase().ends_with(".nca"));
            }
            match (kind, exefs, romfs) {
                (TYPE_BASE | TYPE_UPDATE | TYPE_STANDALONE_UPDATE, Some(exefs), romfs) => {
                    let update = kind != TYPE_BASE;
                    // two programs of one version would unpack into one folder: the first one is the game's main program
                    if !progs.iter().any(|p| p.update == update && p.version == version) {
                        progs.push(Prog { update, version, exefs, romfs });
                    }
                }
                (TYPE_DLC, _, Some(romfs)) if flags & FLAG_ADDON_TITLE_ID != 0 => add_ons.push((u64_at(&c, s + 32)?, romfs)),
                _ => {}
            }
        }
    }
    if progs.is_empty() && native {
        return Err(NATIVE.into());
    }
    Ok(Reader { src: file, tree, index, archive_size, manifest_sha, progs, add_ons, stored: Vec::new(), plain: Vec::with_capacity(1 << 20), zstd: zstd::bulk::Decompressor::new().map_err(|e| e.to_string())? })
}

impl unpack::Package for Reader {
    fn label(&self) -> &'static str {
        LABEL
    }
    fn stamp(&self) -> String {
        format!("nx:{}:{}", self.manifest_sha.iter().map(|b| format!("{:02x}", b)).collect::<String>(), self.archive_size)
    }
    fn programs(&self) -> Vec<Program> {
        self.progs.iter().enumerate().map(|(id, p)| Program { update: p.update, version: p.version, id }).collect()
    }
    fn exefs(&self, p: &Program) -> Result<Vec<(String, usize)>, String> {
        self.tree.tree(self.progs[p.id].exefs)
    }
    fn romfs(&self, p: &Program) -> Result<BTreeMap<String, usize>, String> {
        Ok(match self.progs[p.id].romfs {
            Some(dir) => self.tree.tree(dir)?.into_iter().collect(),
            None => BTreeMap::new(),
        })
    }
    fn add_ons(&self) -> Result<Vec<(u64, Vec<(String, usize)>)>, String> {
        self.add_ons.iter().map(|&(title_id, dir)| Ok((title_id, self.tree.tree(dir)?))).collect()
    }
    fn size(&self, file: usize) -> u64 {
        self.tree.file(file).map_or(0, |f| f.size)
    }
    fn sha256(&self, file: usize) -> [u8; 32] {
        self.tree.file(file).map_or([0; 32], |f| f.sha)
    }
    fn read(&mut self, file: usize, name: &str, w: &mut dyn Write, cancel: &dyn Fn() -> bool) -> Result<(), String> {
        let f = self.tree.file(file)?;
        let mut left = f.size;
        for i in 0..f.blocks as usize {
            if cancel() {
                return Err(unpack::CANCELLED.into());
            }
            let id = u32_at(&self.tree.m, self.tree.refs.0 + 4 * (f.first_ref as usize + i))? as usize;
            let b = self.index.get(24 * id..24 * id + 24).ok_or_else(damaged)?;
            let (at, stored, plain, codec) = (u64_at(b, 0)?, u32_at(b, 8)? as u64, u32_at(b, 12)? as u64, b[16]);
            // every block of a file is one chunk of it, the last one what is left
            let fits = plain == left.min(1 << f.chunk_log2) && stored > 0 && stored <= MAX_STORED && at >= DATA_START && at + stored <= self.archive_size;
            if !fits || codec > 1 || (codec == 0 && stored != plain) {
                return Err(damaged());
            }
            self.stored.resize(stored as usize, 0);
            self.src.seek(SeekFrom::Start(at)).and_then(|_| self.src.read_exact(&mut self.stored)).map_err(|e| format!("{} could not be read from the {}: {}", name, LABEL, e))?;
            let bytes = if codec == 0 {
                &self.stored
            } else {
                // one frame per block, never larger than the chunk: `plain` has room for 1 MiB and no more
                self.plain.clear();
                let n = self.zstd.decompress_to_buffer(&self.stored, &mut self.plain).map_err(|e| format!("{} could not be decompressed: {}", name, e))?;
                if n as u64 != plain {
                    return Err(damaged());
                }
                &self.plain
            };
            w.write_all(bytes).map_err(|e| format!("Cannot write {} (disk full or blocked?): {}", name, e))?;
            left -= plain;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::unpack::tests::{nso, temp};
    use crate::unpack::Package;
    use std::fs;

    /// A snapshot to write: its type, version, add-on title id, and files as (path, bytes, compressed).
    struct Snap {
        kind: u8,
        version: u32,
        add_on: Option<u64>,
        files: Vec<(&'static str, Vec<u8>, bool)>,
    }

    #[derive(Default)]
    struct Folder {
        dirs: BTreeMap<String, usize>,
        files: BTreeMap<String, u32>,
    }

    fn pad(v: &mut Vec<u8>, to: usize) {
        v.resize(v.len().div_ceil(to) * to, 0);
    }

    /// Writes an archive of one application as SPEC.md lays it out (64 KiB chunks), enough of it for this reader.
    fn archive(app: u64, snaps: &[Snap]) -> Vec<u8> {
        let mut out = vec![0u8; DATA_START as usize];
        let (mut folders, mut files, mut refs, mut index, mut checks) = (Vec::<Folder>::new(), Vec::new(), Vec::new(), Vec::new(), Vec::new());
        let mut roots = Vec::new();
        for s in snaps {
            roots.push(folders.len() as u32);
            let root = folders.len();
            folders.push(Folder::default());
            for (path, bytes, packed) in &s.files {
                let mut at = root;
                let mut parts: Vec<&str> = path.split('/').collect();
                let leaf = parts.pop().unwrap();
                for p in parts {
                    at = match folders[at].dirs.get(p) {
                        Some(&d) => d,
                        None => {
                            folders.push(Folder::default());
                            let d = folders.len() - 1;
                            folders[at].dirs.insert(p.to_string(), d);
                            d
                        }
                    };
                }
                folders[at].files.insert(leaf.to_string(), (files.len() / 64) as u32);
                let chunks: Vec<&[u8]> = bytes.chunks(1 << 16).collect();
                files.extend((bytes.len() as u64).to_le_bytes());
                files.extend((refs.len() as u32 / 4).to_le_bytes());
                files.extend((chunks.len() as u32).to_le_bytes());
                files.extend([16u8, 0, 0, 0]);
                files.extend(Sha256::digest(bytes));
                files.extend([0u8; 12]);
                for c in chunks {
                    pad(&mut out, 16);
                    let stored = if *packed { zstd::bulk::compress(c, 3).unwrap() } else { c.to_vec() };
                    refs.extend((index.len() as u32 / 24).to_le_bytes());
                    index.extend((out.len() as u64).to_le_bytes());
                    index.extend((stored.len() as u32).to_le_bytes());
                    index.extend((c.len() as u32).to_le_bytes());
                    index.extend([*packed as u8, 0, 0, 0]);
                    index.extend((checks.len() as u32 / 4).to_le_bytes());
                    for slice in c.chunks(16384) {
                        checks.extend(crc32c(slice).to_le_bytes());
                    }
                    out.extend(stored);
                }
            }
        }
        let data_size = out.len() as u64 - DATA_START;

        // Manifest: folders, files, entries (each folder's in name order, folders in id order), block references, names
        let (mut dirs, mut entries, mut pool) = (Vec::new(), Vec::new(), Vec::new());
        let names_at = 96 + 16 * folders.len() + files.len();
        let entry_count: usize = folders.iter().map(|f| f.dirs.len() + f.files.len()).sum();
        let pool_at = (names_at + 24 * entry_count + refs.len()).div_ceil(8) * 8;
        for f in &folders {
            let mut kids: Vec<(&String, u8, u32)> = f.dirs.iter().map(|(n, &d)| (n, KIND_DIR, d as u32)).chain(f.files.iter().map(|(n, &i)| (n, KIND_FILE, i))).collect();
            kids.sort();
            dirs.extend((entries.len() as u32 / 24).to_le_bytes());
            dirs.extend((kids.len() as u32).to_le_bytes());
            dirs.extend([0u8; 8]);
            for (name, kind, target) in kids {
                entries.extend((pool_at as u64 + pool.len() as u64).to_le_bytes());
                entries.extend((name.len() as u32).to_le_bytes());
                entries.extend(target.to_le_bytes());
                entries.extend([kind, 0, 0, 0, 0, 0, 0, 0]);
                pool.extend(name.bytes());
            }
        }
        let mut m = b"NXMF".to_vec();
        m.extend([1u8, 0, 96, 0]);
        m.extend([0u8; 8]);
        for n in [folders.len(), files.len() / 64, entry_count, refs.len() / 4] {
            m.extend((n as u32).to_le_bytes());
        }
        for at in [96, 96 + dirs.len(), names_at, names_at + entries.len(), pool_at, pool.len()] {
            m.extend((at as u64).to_le_bytes());
        }
        m.extend([0u8; 16]);
        for part in [&dirs, &files, &entries, &refs] {
            m.extend(part.iter());
        }
        pad(&mut m, 8);
        m.extend(&pool);
        let len = m.len() as u64;
        m[8..16].copy_from_slice(&len.to_le_bytes());

        // Catalog: one application, one program, the snapshots
        let mut c = b"NXCT".to_vec();
        c.extend([1u8, 0, 128, 0]);
        c.extend([0u8; 8]);
        for n in [0u32, 1, 1, snaps.len() as u32, 0, 0, 0, 0] {
            c.extend(n.to_le_bytes());
        }
        for at in [128u64, 208, 272, 0, 0, 0, 272 + 120 * snaps.len() as u64, 0] {
            c.extend(at.to_le_bytes());
        }
        c.extend([0u8; 16]);
        c.extend(app.to_le_bytes());
        c.extend([2u8, 1, 0, 0]);
        for n in [0u32, 1, 0, snaps.len() as u32] {
            c.extend(n.to_le_bytes());
        }
        c.extend([0u8; 52]);
        c.extend(app.to_le_bytes());
        c.extend([0u8; 56]);
        for (s, root) in snaps.iter().zip(&roots) {
            c.extend(if s.add_on.is_some() { ABSENT_PROGRAM } else { 0u32 }.to_le_bytes());
            c.extend([s.kind, if s.add_on.is_some() { FLAG_ADDON_TITLE_ID } else { 0 }, 0, 0]);
            c.extend(s.version.to_le_bytes());
            c.extend(root.to_le_bytes());
            c.extend([0u8; 16]);
            c.extend(s.add_on.unwrap_or(0).to_le_bytes());
            c.extend([0u8; 80]);
        }
        let len = c.len() as u64;
        c[8..16].copy_from_slice(&len.to_le_bytes());

        // the tail, then the header in both pages
        let mut at = Vec::new();
        for part in [&c, &m, &index, &checks] {
            pad(&mut out, 16);
            at.push(out.len() as u64);
            out.extend(part.iter());
        }
        let mut h = MAGIC.to_vec();
        h.extend([0u8, 0, 2, 0]);
        h.extend(256u32.to_le_bytes());
        h.extend([1u8, 0, 0, 0]);
        let blocks = index.len() as u64 / 24;
        let some = |v: u64| if blocks == 0 { 0 } else { v };
        for v in [out.len() as u64, at[0], c.len() as u64, 0, 0, some(DATA_START), data_size, at[1], m.len() as u64, some(at[2]), blocks, some(at[3]), checks.len() as u64] {
            h.extend(v.to_le_bytes());
        }
        h.extend(Sha256::digest(&c));
        h.extend(Sha256::digest(&m));
        h.extend(Sha256::new().chain_update(&index).chain_update(&checks).finalize());
        h.extend(0u64.to_le_bytes());
        h.extend(16384u32.to_le_bytes());
        h.extend([0u8; 24]);
        h.extend(crc32c(&h).to_le_bytes());
        out[..HEADER].copy_from_slice(&h);
        out[PAGE..PAGE + HEADER].copy_from_slice(&h);
        out
    }

    const ABSENT_PROGRAM: u32 = 0xFFFF_FFFF;
    const GAME: u64 = crate::saveimport::PROGRAM_ID;

    fn noise(n: usize) -> Vec<u8> {
        (0..n).map(|i| (i * 31 % 251) as u8).collect()
    }

    /// A base game and an update that holds the whole game, as nxconvert writes them from extracted files.
    fn game() -> Vec<Snap> {
        let big = noise(150_000);
        vec![
            Snap { kind: TYPE_BASE, version: 0, add_on: None, files: vec![("exefs/main", nso(1), false), ("romfs/a/old.bin", b"old".to_vec(), false)] },
            Snap {
                kind: TYPE_UPDATE,
                version: 655360,
                add_on: None,
                files: vec![("exefs/main", nso(2), false), ("exefs/main.npdm", b"META".to_vec(), false), ("romfs/a/big.bin", big, true), ("romfs/empty", Vec::new(), false), ("data/x", b"x".to_vec(), false)],
            },
            Snap { kind: TYPE_DLC, version: 0, add_on: Some(0x0400A3A01EE4D001), files: vec![("romfs/sub/a.bin", b"dlc".to_vec(), true)] },
        ]
    }

    fn opened(tag: &str, bytes: &[u8]) -> Result<Reader, String> {
        let p = temp(tag).with_extension("nx");
        fs::write(&p, bytes).unwrap();
        let r = open(File::open(&p).unwrap());
        let _ = fs::remove_file(&p);
        r
    }

    #[test]
    fn crc32c_matches_the_standard_check_value() {
        assert_eq!(crc32c(b"123456789"), 0xe306_9283);
    }

    #[test]
    fn an_archive_of_extracted_files_unpacks_like_a_dump_folder() {
        let p = temp("nx-file").with_extension("nx");
        fs::write(&p, archive(GAME, &game())).unwrap();
        let out = temp("nx-out");
        let mut r = open(File::open(&p).unwrap()).unwrap();
        let want = "02".repeat(20);
        unpack::run(&mut r, &out, &mut |_, _, _| {}, &|| false, Some(&want), &|_| None).unwrap();
        let up = out.join("updates/655360");
        assert_eq!(fs::read(up.join("romfs/a/big.bin")).unwrap(), noise(150_000), "three blocks, each its own Zstandard frame");
        assert_eq!(fs::read(up.join("romfs/empty")).unwrap(), b"");
        assert_eq!(fs::read(up.join("exefs/main.npdm")).unwrap(), b"META");
        assert!(!up.join("data").exists() && !out.join("base/romfs").exists());
        assert_eq!(fs::read(out.join("aoc/0400A3A01EE4D001/romfs/sub/a.bin")).unwrap(), b"dlc");
        let _ = fs::remove_dir_all(&out);
        let _ = fs::remove_file(&p);
    }

    #[test]
    fn damage_is_found_when_the_archive_is_opened_or_its_file_is_read() {
        let good = archive(GAME, &game());
        let tail = good.len() - 1;
        let flipped = |at: &[usize]| {
            let mut bad = good.clone();
            at.iter().for_each(|&i| bad[i] ^= 1);
            opened("nx-bad", &bad).map(|_| ())
        };
        // one damaged header page (its magic, its fields, or bytes after it) leaves the other one
        for at in [0, 100, PAGE + 300] {
            assert_eq!(flipped(&[at]), Ok(()), "byte {at}");
        }
        // both pages, or a byte of the block index (the last thing in the file), and the archive is refused
        assert_eq!(flipped(&[100, PAGE + 300]), Err(DAMAGED.to_string()));
        assert_eq!(flipped(&[tail]), Err(DAMAGED.to_string()));
        assert_eq!(opened("nx-short", &good[..tail]).err().unwrap(), DAMAGED);
        assert_eq!(opened("nx-other", &vec![7u8; 9000]).err().unwrap(), NOT_NX);

        // a changed data byte passes the open (blocks are not read then) and fails the file's own checksum
        let mut bad = good.clone();
        bad[DATA_START as usize] ^= 1;
        let p = temp("nx-data").with_extension("nx");
        fs::write(&p, bad).unwrap();
        let out = temp("nx-data-out");
        let e = unpack::run(&mut open(File::open(&p).unwrap()).unwrap(), &out, &mut |_, _, _| {}, &|| false, None, &|_| None).unwrap_err();
        assert!(e.contains("does not match its checksum: the .nx is damaged"), "{e}");
        let _ = fs::remove_dir_all(&out);
        let _ = fs::remove_file(&p);
    }

    #[test]
    fn an_archive_of_nca_packages_is_refused_in_words() {
        let nca = vec![Snap { kind: TYPE_BASE, version: 0, add_on: None, files: vec![("0123456789abcdef.nca", b"NCA3".to_vec(), false)] }];
        assert_eq!(opened("nx-nca", &archive(GAME, &nca)).err().unwrap(), NATIVE);
        // a name that would leave the folder it is unpacked into never gets that far
        let escape = vec![Snap { kind: TYPE_BASE, version: 0, add_on: None, files: vec![("exefs/..", nso(1), false)] }];
        let r = opened("nx-escape", &archive(GAME, &escape)).unwrap();
        assert!(r.exefs(&r.programs()[0]).unwrap_err().contains("cannot be used here"));
    }
}
