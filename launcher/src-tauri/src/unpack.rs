// SPDX-License-Identifier: GPL-3.0-or-later
//! Unpacks a game package (a `.nup`, nup.rs, or an `.nx`, nx.rs) into the folder layout the rest of the launcher
//! expects: `<out>/base/{exefs,romfs}` and `<out>/updates/<version>/{exefs,romfs}`. Only the exefs and romfs of each
//! program are written. Add-ons (DLC) with a RomFS go to `<out>/aoc/<16-digit hex id>/romfs/`, the layout dump.rs
//! finds for pkp's `[fs] aoc_dir` (#236).
//!
//! A format reads its own file and answers as a `Package`; the order of work here is the same for both, so a wrong
//! or damaged package fails in seconds and not after 7 GB: every program's ExeFS (small) and the main build ID check;
//! then the free-space check; then the RomFS of the chosen program only. Every file is written to `<name>.part` and
//! renamed once its size and SHA-256 match the package's record.
//!
//! The checks (per-file hash, `.part` + rename, free space) follow pokopia-native (MIT), nxs2.py nup_unpack @7c3655d.

use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

/// Marks a finished unpack and holds the package's stamp. Named for the first package format; every format uses it.
const MARKER: &str = ".nup_done";
const WORKING: &str = ".unpacking";
/// Free space kept beyond the unpacked size.
const SPARE: u64 = 2 << 30;
pub const CANCELLED: &str = "Unpacking was cancelled.";

/// A base game or an update of a package that has an ExeFS.
#[derive(Clone, Copy)]
pub struct Program {
    pub update: bool,
    pub version: u32,
    /// The package's own handle for it.
    pub id: usize,
}

/// What a package format answers. Files are named by the format's own index; a path is relative, with '/', and made
/// of names that passed `checked_name`.
pub trait Package {
    /// ".nup" or ".nx", for the player's messages.
    fn label(&self) -> &'static str;
    /// Names this very file: a finished unpack with the same stamp is reused.
    fn stamp(&self) -> String;
    fn programs(&self) -> Vec<Program>;
    fn exefs(&self, p: &Program) -> Result<Vec<(String, usize)>, String>;
    /// The program's RomFS as the game needs it: whole, with what an update leaves to the base game under it.
    fn romfs(&self, p: &Program) -> Result<BTreeMap<String, usize>, String>;
    /// The RomFS files of every add-on: (title id, files).
    fn add_ons(&self) -> Result<Vec<(u64, Vec<(String, usize)>)>, String>;
    fn size(&self, file: usize) -> u64;
    fn sha256(&self, file: usize) -> [u8; 32];
    /// Writes the file's bytes to `w`, never more than `size(file)` + 1 of them (guards against decompression bombs).
    fn read(&mut self, file: usize, name: &str, w: &mut dyn Write, cancel: &dyn Fn() -> bool) -> Result<(), String>;
}

/// Names in a package are labels (checked when a folder is walked), never paths: no separators, no "." or "..", nothing Windows reads specially.
pub fn checked_name<'a>(n: &'a str, label: &str) -> Result<&'a str, String> {
    if n.is_empty() || n == "." || n == ".." || n.contains(['/', '\\', ':', '\0', '<', '>', '"', '|', '?', '*']) || n.chars().any(|c| (c as u32) < 0x20) {
        return Err(format!("The {} contains a file name that cannot be used here: {:?}", label, n));
    }
    Ok(n)
}

/// Copies `r` to `w` in pieces, polling `cancel` between them.
pub fn copy(r: &mut dyn Read, w: &mut dyn Write, cancel: &dyn Fn() -> bool, name: &str, label: &str) -> Result<(), String> {
    let mut buf = vec![0u8; 1 << 20];
    loop {
        if cancel() {
            return Err(CANCELLED.into());
        }
        let n = r.read(&mut buf).map_err(|e| format!("{} could not be read from the {}: {}", name, label, e))?;
        if n == 0 {
            return Ok(());
        }
        w.write_all(&buf[..n]).map_err(|e| format!("Cannot write {} (disk full or blocked?): {}", name, e))?;
    }
}

/// Writes through to `inner` and hashes what passes.
struct HashWriter<W: Write> {
    inner: W,
    hash: Sha256,
    len: u64,
}
impl<W: Write> Write for HashWriter<W> {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        let n = self.inner.write(b)?;
        self.hash.update(&b[..n]);
        self.len += n as u64;
        Ok(n)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

struct Run<'a> {
    pkg: &'a mut dyn Package,
    done: u64,
    total: u64,
    progress: &'a mut dyn FnMut(u64, u64, &str),
    cancel: &'a dyn Fn() -> bool,
}

impl Run<'_> {
    /// One file to `dest`: written to `<dest>.part`, renamed once its size and SHA-256 match the record.
    fn file(&mut self, fi: usize, dest: &Path) -> Result<(), String> {
        let name = dest.file_name().unwrap_or_default().to_string_lossy().into_owned();
        if (self.cancel)() {
            return Err(CANCELLED.into());
        }
        (self.progress)(self.done, self.total, &name);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("Cannot create {}: {}", parent.display(), e))?;
        }
        let mut tmp_name = dest.file_name().unwrap_or_default().to_os_string();
        tmp_name.push(".part");
        let tmp = dest.with_file_name(tmp_name);
        let res = self.write_part(fi, &name, &tmp);
        if res.is_err() {
            let _ = fs::remove_file(&tmp);
            return res;
        }
        fs::rename(&tmp, dest).map_err(|e| {
            let _ = fs::remove_file(&tmp);
            format!("Cannot write {}: {}", dest.display(), e)
        })?;
        self.done += self.pkg.size(fi);
        Ok(())
    }

    fn write_part(&mut self, fi: usize, name: &str, tmp: &Path) -> Result<(), String> {
        let disk = |e: std::io::Error| format!("Cannot write {} (disk full or blocked?): {}", tmp.display(), e);
        let mut out = HashWriter { inner: std::io::BufWriter::with_capacity(1 << 20, File::create(tmp).map_err(disk)?), hash: Sha256::new(), len: 0 };
        self.pkg.read(fi, name, &mut out, self.cancel)?;
        out.flush().map_err(disk)?;
        if out.len != self.pkg.size(fi) || out.hash.finalize().as_slice() != self.pkg.sha256(fi) {
            return Err(format!("{} does not match its checksum: the {} is damaged. Download it again.", name, self.pkg.label()));
        }
        Ok(())
    }
}

/// Unpacks the package at `path` into `out`. A finished unpack of the same package is reused. `progress(done, total,
/// current file)` is called per file; `cancel` is polled during the unpack. Returns `out`.
pub fn extract(path: &Path, out: &Path, progress: &mut dyn FnMut(u64, u64, &str), cancel: &dyn Fn() -> bool) -> Result<PathBuf, String> {
    let mut pkg = open(path)?;
    let want = crate::dump::supported().map(|(id, _)| id);
    run(pkg.as_mut(), out, progress, cancel, want.as_deref(), &crate::preflight::free_bytes)
}

/// The package behind `path`, by what the file starts with; a file that is neither is answered by the format its name asks for.
fn open(path: &Path) -> Result<Box<dyn Package>, String> {
    let mut file = File::open(path).map_err(|e| format!("Cannot open {}: {}", path.display(), e))?;
    let mut magic = [0u8; 4];
    let got = file.read(&mut magic).map_err(|e| format!("Cannot open {}: {}", path.display(), e))?;
    file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
    let named_nx = path.extension().map_or(false, |e| e.eq_ignore_ascii_case("nx"));
    let nx = match &magic[..got] {
        m if m == crate::nx::MAGIC => true,
        m if m == crate::nup::MAGIC => false,
        _ => named_nx,
    };
    Ok(if nx { Box::new(crate::nx::open(file)?) } else { Box::new(crate::nup::open(file)?) })
}

pub(crate) fn run(
    pkg: &mut dyn Package,
    out: &Path,
    progress: &mut dyn FnMut(u64, u64, &str),
    cancel: &dyn Fn() -> bool,
    want_build: Option<&str>,
    free: &dyn Fn(&Path) -> Option<u64>,
) -> Result<PathBuf, String> {
    let label = pkg.label();
    let stamp = pkg.stamp();
    if fs::read_to_string(out.join(MARKER)).map(|s| s.trim() == stamp).unwrap_or(false) {
        // an unpack from a launcher that skipped add-ons gets them now, without unpacking the game again
        if !fs::read_to_string(out.join("aoc").join(MARKER)).map(|s| s.trim() == stamp).unwrap_or(false) {
            let mut run = Run { pkg, done: 0, total: 0, progress, cancel };
            add_ons(&mut run, out, &stamp)?;
        }
        return Ok(out.to_path_buf());
    }
    // programs, newest update first, the base last
    let mut progs = pkg.programs();
    progs.sort_by_key(|p| std::cmp::Reverse((p.update, p.version)));
    if progs.is_empty() {
        return Err(format!("This {} has no base game or update in it.", label));
    }

    // only a folder this code made is ever removed
    if out.exists() {
        let ours = out.join(MARKER).is_file() || out.join(WORKING).is_file() || fs::read_dir(out).map_err(|e| e.to_string())?.next().is_none();
        if !ours {
            return Err(format!("{} already exists and was not made by the launcher. Move or delete it, then try again.", out.display()));
        }
        fs::remove_dir_all(out).map_err(|e| format!("Cannot clear {}: {}", out.display(), e))?;
    }
    fs::create_dir_all(out).map_err(|e| format!("Cannot create {}: {}", out.display(), e))?;
    fs::write(out.join(WORKING), &stamp).map_err(|e| e.to_string())?;

    let mut run = Run { pkg, done: 0, total: 0, progress, cancel };
    let root_of = |p: &Program| if p.update { out.join("updates").join(p.version.to_string()) } else { out.join("base") };

    // ExeFS of every program first (small), then the version check: nothing big is written for a wrong package
    let mut chosen: Option<&Program> = None;
    let mut seen = Vec::new();
    for p in &progs {
        let root = root_of(p);
        for (rel, fi) in run.pkg.exefs(p)? {
            run.file(fi, &root.join("exefs").join(&rel))?;
        }
        let bid = crate::dump::main_build_id(&root.join("exefs").join("main"));
        let name = if p.update { format!("update {}", p.version) } else { "base game".to_string() };
        seen.push(format!("{} (build {})", name, bid.as_deref().unwrap_or("none")));
        if chosen.is_none() && (want_build.is_none() || bid.as_deref() == want_build) {
            chosen = Some(p);
        }
    }
    let p = chosen.ok_or_else(|| {
        format!("This {} is not the game version this launcher supports (build {}). It holds: {}.", label, want_build.unwrap_or("?"), seen.join(", "))
    })?;

    let files = run.pkg.romfs(p)?;
    if files.is_empty() {
        return Err(format!("The game program in this {} has no RomFS (game data).", label));
    }
    let aoc_bytes: u64 = add_on_files(&*run.pkg)?.iter().map(|(_, fi)| run.pkg.size(*fi)).sum();
    let need: u64 = files.values().map(|&fi| run.pkg.size(fi)).sum::<u64>() + aoc_bytes;
    if let Some(avail) = free(out) {
        if avail < need.saturating_add(SPARE) {
            let gb = |b: u64| (b as f64 / (1u64 << 30) as f64).ceil() as u64;
            return Err(format!(
                "Not enough free disk space to unpack the game: about {} GB are needed on the drive of {}, {} GB are free.",
                gb(need.saturating_add(SPARE)),
                out.display(),
                avail >> 30
            ));
        }
    }
    run.total = need;
    run.done = 0;
    let romfs = root_of(p).join("romfs");
    fs::create_dir_all(&romfs).map_err(|e| format!("Cannot create {}: {}", romfs.display(), e))?;
    for (rel, fi) in &files {
        run.file(*fi, &romfs.join(rel))?;
    }
    add_ons(&mut run, out, &stamp)?;
    (run.progress)(need, need, "");
    fs::write(out.join(MARKER), stamp).map_err(|e| e.to_string())?;
    let _ = fs::remove_file(out.join(WORKING));
    Ok(out.to_path_buf())
}

/// The RomFS files of every add-on as (path below `<out>`, file index): `aoc/<ID>/romfs/<rel>`.
/// pkp decides which ids belong to this title.
fn add_on_files(pkg: &dyn Package) -> Result<Vec<(PathBuf, usize)>, String> {
    let mut out = Vec::new();
    for (title_id, files) in pkg.add_ons()? {
        let root = Path::new("aoc").join(format!("{:016X}", title_id)).join("romfs");
        out.extend(files.into_iter().map(|(rel, fi)| (root.join(rel), fi)));
    }
    Ok(out)
}

/// Writes the add-ons into `<out>/aoc` and marks them done with `stamp`. The folder is made even with none, so a
/// player can see where an add-on dumped on its own goes.
fn add_ons(run: &mut Run, out: &Path, stamp: &str) -> Result<(), String> {
    let aoc = out.join("aoc");
    fs::create_dir_all(&aoc).map_err(|e| format!("Cannot create {}: {}", aoc.display(), e))?;
    let files = add_on_files(&*run.pkg)?;
    if run.total == 0 {
        run.total = files.iter().map(|(_, fi)| run.pkg.size(*fi)).sum();
    }
    for (rel, fi) in files {
        run.file(fi, &out.join(rel))?;
    }
    fs::write(aoc.join(MARKER), stamp).map_err(|e| e.to_string())
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A package of files held in memory: (program, exefs, romfs) and add-ons, every file a byte string.
    #[derive(Default)]
    pub struct Mem {
        pub files: Vec<Vec<u8>>,
        pub progs: Vec<(Program, Vec<(String, usize)>, Vec<(String, usize)>)>,
        pub aoc: Vec<(u64, Vec<(String, usize)>)>,
    }

    impl Mem {
        pub fn add(&mut self, bytes: &[u8]) -> usize {
            self.files.push(bytes.to_vec());
            self.files.len() - 1
        }
    }

    impl Package for Mem {
        fn label(&self) -> &'static str {
            ".nup"
        }
        fn stamp(&self) -> String {
            "mem:1".into()
        }
        fn programs(&self) -> Vec<Program> {
            self.progs.iter().map(|p| p.0).collect()
        }
        fn exefs(&self, p: &Program) -> Result<Vec<(String, usize)>, String> {
            Ok(self.progs[p.id].1.clone())
        }
        fn romfs(&self, p: &Program) -> Result<BTreeMap<String, usize>, String> {
            Ok(self.progs[p.id].2.iter().cloned().collect())
        }
        fn add_ons(&self) -> Result<Vec<(u64, Vec<(String, usize)>)>, String> {
            Ok(self.aoc.clone())
        }
        fn size(&self, file: usize) -> u64 {
            self.files[file].len() as u64
        }
        fn sha256(&self, file: usize) -> [u8; 32] {
            Sha256::digest(&self.files[file]).into()
        }
        fn read(&mut self, file: usize, name: &str, w: &mut dyn Write, _: &dyn Fn() -> bool) -> Result<(), String> {
            w.write_all(&self.files[file]).map_err(|e| format!("Cannot write {}: {}", name, e))
        }
    }

    /// An NSO header with `build` as its build ID, as dump::main_build_id reads it.
    pub fn nso(build: u8) -> Vec<u8> {
        let mut v = vec![0u8; 0x60];
        v[..4].copy_from_slice(b"NSO0");
        v[0x40..0x54].fill(build);
        v
    }

    pub fn temp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-unpack-{}-{}", tag, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        d
    }

    fn game() -> Mem {
        let mut m = Mem::default();
        let (old, new, data, dlc) = (m.add(&nso(1)), m.add(&nso(2)), m.add(b"data"), m.add(b"dlc"));
        m.progs.push((Program { update: false, version: 0, id: 0 }, vec![("main".into(), old)], vec![("a/old.bin".into(), data)]));
        m.progs.push((Program { update: true, version: 7, id: 1 }, vec![("main".into(), new)], vec![("a/b.bin".into(), data)]));
        m.aoc.push((0x0400A3A01EE4D001, vec![("sub/a.bin".into(), dlc)]));
        m
    }

    #[test]
    fn the_wanted_build_is_unpacked_with_its_add_ons() {
        let out = temp("run");
        let want = "02".repeat(20);
        let got = run(&mut game(), &out, &mut |_, _, _| {}, &|| false, Some(&want), &|_| None).unwrap();
        assert_eq!(got, out);
        assert_eq!(fs::read(out.join("updates/7/romfs/a/b.bin")).unwrap(), b"data");
        assert!(out.join("base/exefs/main").is_file(), "every program's exefs is written for the version check");
        assert!(!out.join("base/romfs").exists(), "only the chosen program's romfs is written");
        assert_eq!(fs::read(out.join("aoc/0400A3A01EE4D001/romfs/sub/a.bin")).unwrap(), b"dlc");
        assert_eq!(fs::read_to_string(out.join(MARKER)).unwrap(), "mem:1");
        assert!(!out.join(WORKING).exists());
        // a finished unpack of the same package is reused: nothing is read again
        let mut empty = Mem::default();
        assert_eq!(run(&mut empty, &out, &mut |_, _, _| {}, &|| false, Some(&want), &|_| None).unwrap(), out);
        let _ = fs::remove_dir_all(&out);
    }

    #[test]
    fn a_wrong_version_a_full_disk_and_a_foreign_folder_are_refused() {
        let out = temp("refuse");
        let e = run(&mut game(), &out, &mut |_, _, _| {}, &|| false, Some("ff"), &|_| None).unwrap_err();
        assert!(e.starts_with("This .nup is not the game version") && e.contains("update 7 (build 0202"), "{e}");
        let e = run(&mut game(), &out, &mut |_, _, _| {}, &|| false, None, &|_| Some(0)).unwrap_err();
        assert!(e.starts_with("Not enough free disk space"), "{e}");
        let e = run(&mut game(), &out, &mut |_, _, _| {}, &|| true, None, &|_| None).unwrap_err();
        assert_eq!(e, CANCELLED);
        let _ = fs::remove_dir_all(&out);
        fs::create_dir_all(&out).unwrap();
        fs::write(out.join("mine.txt"), "x").unwrap();
        let e = run(&mut game(), &out, &mut |_, _, _| {}, &|| false, None, &|_| None).unwrap_err();
        assert!(e.contains("was not made by the launcher"), "{e}");
        let _ = fs::remove_dir_all(&out);
    }

    #[test]
    fn a_file_that_differs_from_its_record_is_refused() {
        struct Lying(Mem);
        impl Package for Lying {
            fn label(&self) -> &'static str { ".nup" }
            fn stamp(&self) -> String { self.0.stamp() }
            fn programs(&self) -> Vec<Program> { self.0.programs() }
            fn exefs(&self, p: &Program) -> Result<Vec<(String, usize)>, String> { self.0.exefs(p) }
            fn romfs(&self, p: &Program) -> Result<BTreeMap<String, usize>, String> { self.0.romfs(p) }
            fn add_ons(&self) -> Result<Vec<(u64, Vec<(String, usize)>)>, String> { self.0.add_ons() }
            fn size(&self, file: usize) -> u64 { self.0.size(file) }
            fn sha256(&self, _: usize) -> [u8; 32] { [0; 32] }
            fn read(&mut self, file: usize, name: &str, w: &mut dyn Write, c: &dyn Fn() -> bool) -> Result<(), String> { self.0.read(file, name, w, c) }
        }
        let out = temp("lying");
        let e = run(&mut Lying(game()), &out, &mut |_, _, _| {}, &|| false, None, &|_| None).unwrap_err();
        assert!(e.contains("does not match its checksum"), "{e}");
        assert!(!out.join("updates/7/exefs/main").exists() && !out.join("updates/7/exefs/main.part").exists());
        let _ = fs::remove_dir_all(&out);
    }
}
