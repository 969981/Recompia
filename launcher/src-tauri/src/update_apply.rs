// SPDX-License-Identifier: GPL-3.0-or-later
//! The swap of an update (step 3 of update.rs): each file of the unpacked package moves its old self to `.update/backup/` and takes
//! its place, the launcher last. Files the package does not hold are never looked at, and it can never replace `KEEP` even if a
//! release shipped one. Every step records its inverse; a failure runs them backwards. The `applying` marker covers a power cut:
//! the next start restores the backup instead of deleting it (`recover`).
//! Plain paths and no AppHandle: the tests below build a fake install and run it.

use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};
use std::time::Duration;

/// In `.update/` while files move: a start that finds it restores the backup.
pub const MARKER: &str = "applying";

/// Top-level names that belong to the player. make-release.ps1 refuses to ship most of them; `apply` skips them all the same.
/// Never replaced or removed by an update: the player's save, the copies of it (`save-backups`, savebackup.rs; `rollback-backups`, rollback.rs) and settings.
const KEEP: [&str; 10] = ["save", "save-backups", "pkp.ini", "profiles.json", "controls.ini", "shader_cache", "build", "nup_dump", "rollback-backups", crate::update::UPDATE_DIR];

/// Windows: a virus scanner holds a file it just saw (the extracted launcher above all) for a moment, so a rename waits a little.
fn retryable(e: &io::Error) -> bool {
    cfg!(windows) && matches!(e.raw_os_error(), Some(5 | 32 | 33)) // access denied, sharing and lock violation
}

fn mv(from: &Path, to: &Path) -> io::Result<()> {
    let mut tries = 0;
    loop {
        match fs::rename(from, to) {
            Err(e) if tries < 20 && retryable(&e) => {
                tries += 1;
                std::thread::sleep(Duration::from_millis(150));
            }
            r => return r,
        }
    }
}

pub fn exists(p: &Path) -> bool {
    fs::symlink_metadata(p).is_ok()
}

pub fn with_old(p: &Path) -> PathBuf {
    let mut s = p.as_os_str().to_os_string();
    s.push(".old");
    PathBuf::from(s)
}

/// Every file (and symlink) under `dir`, relative to it, sorted.
fn walk(dir: &Path) -> io::Result<Vec<PathBuf>> {
    let (mut out, mut stack) = (Vec::new(), vec![PathBuf::new()]);
    while let Some(rel) = stack.pop() {
        for e in fs::read_dir(dir.join(&rel))? {
            let e = e?;
            let r = rel.join(e.file_name());
            if e.file_type()?.is_dir() {
                stack.push(r);
            } else {
                out.push(r);
            }
        }
    }
    out.sort();
    Ok(out)
}

fn kept(rel: &Path) -> bool {
    match rel.components().next() {
        Some(Component::Normal(top)) => KEEP.iter().any(|k| top.to_string_lossy().eq_ignore_ascii_case(k)),
        _ => true,
    }
}

/// One undo step: the inverse of something `apply` did.
#[derive(Debug)]
enum Undo {
    Move { from: PathBuf, to: PathBuf },
    Remove(PathBuf),
    RemoveDir(PathBuf),
}

/// Runs the steps backwards. Returns what could not be put back.
fn undo_all(steps: Vec<Undo>) -> Vec<String> {
    let mut failed = Vec::new();
    for s in steps.into_iter().rev() {
        match s {
            Undo::Move { from, to } => {
                if let Err(e) = mv(&from, &to) {
                    failed.push(format!("{}: {}", to.display(), e));
                }
            }
            Undo::Remove(p) => {
                if let Err(e) = fs::remove_file(&p) {
                    if e.kind() != io::ErrorKind::NotFound {
                        failed.push(format!("{}: {}", p.display(), e));
                    }
                }
            }
            Undo::RemoveDir(p) => drop(fs::remove_dir(&p)), // only an empty one goes; an extra empty folder would harm nothing
        }
    }
    failed
}

pub fn rollback_message(failed: &[String]) -> String {
    format!("{} files could not be put back (first: {}). Close the launcher and start it again; if this stays, extract the release again.", failed.len(), failed[0])
}

/// What `apply` works on. Plain paths, so the tests run it on a fake install.
pub struct Job<'a> {
    /// the install: the launcher's folder
    pub root: &'a Path,
    /// the unpacked `PokopiaRecomp-<v>/`
    pub package: &'a Path,
    /// `.update/`, on the same drive as `root`
    pub work: &'a Path,
    /// the launcher's name in the package
    pub launcher_name: &'a str,
    /// the launcher's path in the install, which the player may have renamed
    pub launcher: &'a Path,
    /// the old pkp_rt.dll of a custom game folder: pkp.exe there loads this copy
    pub game_dll: Option<PathBuf>,
}

/// A finished swap, which can still be undone.
#[derive(Debug, Default)]
pub struct Applied(Vec<Undo>);

impl Applied {
    /// Runs the swap backwards. Returns what could not be put back.
    pub fn undo(self) -> Vec<String> {
        undo_all(self.0)
    }
}

fn make_parents(dir: &Path, undo: &mut Vec<Undo>) -> io::Result<()> {
    let missing: Vec<&Path> = dir.ancestors().take_while(|a| !exists(a)).collect();
    for d in missing.into_iter().rev() {
        fs::create_dir(d)?;
        undo.push(Undo::RemoveDir(d.to_path_buf()));
    }
    Ok(())
}

fn parent_of(p: &Path) -> &Path {
    p.parent().unwrap_or(Path::new(""))
}

type Mover<'m> = &'m mut dyn FnMut(&Path, &Path) -> io::Result<()>;

fn step(from: &Path, to: &Path, undo: &mut Vec<Undo>, mover: &mut Mover) -> io::Result<()> {
    mover(from, to)?;
    undo.push(Undo::Move { from: to.to_path_buf(), to: from.to_path_buf() });
    Ok(())
}

fn swap(job: &Job, undo: &mut Vec<Undo>, files: &[PathBuf], report: &mut dyn FnMut(&str, u64, u64), mover: &mut Mover) -> io::Result<()> {
    let backup = job.work.join("backup");
    let total = files.len() as u64 + 1;
    for (i, rel) in files.iter().enumerate() {
        report("apply", i as u64, total);
        let dst = job.root.join(rel);
        make_parents(parent_of(&dst), undo)?;
        if exists(&dst) {
            let bak = backup.join(rel);
            fs::create_dir_all(parent_of(&bak))?; // inside .update/, removed with it
            step(&dst, &bak, undo, mover)?;
        }
        step(&job.package.join(rel), &dst, undo, mover)?;
    }
    if let Some(dst) = &job.game_dll {
        let src = crate::host::runtime_lib(job.root);
        if src.is_file() {
            // possibly another drive, and the install keeps its own: a copy, with the old file set aside beside it
            let old = with_old(dst);
            let _ = fs::remove_file(&old);
            step(dst, &old, undo, mover)?;
            undo.push(Undo::Remove(dst.clone()));
            fs::copy(&src, dst)?;
        }
    }
    // last, so an install cut short by a power failure still starts the launcher that can restore it
    let (src, dst) = (job.package.join(job.launcher_name), job.launcher);
    let old = with_old(dst);
    let _ = fs::remove_file(&old);
    if exists(dst) {
        step(dst, &old, undo, mover)?;
    }
    step(&src, dst, undo, mover)?;
    report("apply", total, total);
    Ok(())
}

pub fn apply(job: &Job, report: &mut dyn FnMut(&str, u64, u64)) -> Result<Applied, String> {
    apply_with(job, report, &mut |a, b| mv(a, b))
}

/// `apply` with the move of the forward steps passed in, so a test can fail any one of them.
fn apply_with(job: &Job, report: &mut dyn FnMut(&str, u64, u64), mut mover: Mover) -> Result<Applied, String> {
    let unreadable = |e: io::Error| format!("Could not read the unpacked update: {}. Nothing was changed.", e);
    let files: Vec<PathBuf> = walk(job.package).map_err(unreadable)?.into_iter().filter(|r| !kept(r) && r != Path::new(job.launcher_name)).collect();
    // a folder where the package has a file: the one conflict moving aside must not resolve, it would take the folder's content along
    if let Some(r) = files.iter().find(|r| job.root.join(r).is_dir()) {
        return Err(format!("{} is a folder in your game folder, but the update has a file of that name. Nothing was changed.", r.display()));
    }
    let marker = job.work.join(MARKER);
    fs::write(&marker, "").map_err(|e| format!("Could not write in the update folder: {}. Nothing was changed.", e))?;
    let mut undo = Vec::new();
    match swap(job, &mut undo, &files, report, &mut mover) {
        Ok(()) => {
            let _ = fs::remove_file(&marker);
            Ok(Applied(undo))
        }
        Err(e) => {
            let failed = undo_all(undo);
            if failed.is_empty() {
                let _ = fs::remove_file(&marker);
                Err(format!("The update could not be put in place: {}. Everything was restored.", e))
            } else {
                Err(format!("The update could not be put in place: {}. {}", e, rollback_message(&failed))) // the marker stays: the next start tries again
            }
        }
    }
}

/// A start found the `applying` marker: a swap was cut off. Puts the backup back over the install. Returns how many files stayed behind.
/// ponytail: only the backup. A launcher cut off in its own last two moves, or a custom folder's dll, stays new: both still work, and
/// the old pkp.exe with a new runtime is the state every update leaves until the rebuild anyway.
pub fn recover(root: &Path, work: &Path) -> usize {
    let backup = work.join("backup");
    let files = walk(&backup).unwrap_or_default();
    files.iter().filter(|rel| mv(&backup.join(rel), &root.join(rel)).is_err()).count()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_update_never_touches_the_players_save_backups() {
        assert!(KEEP.contains(&"save-backups") && KEEP.contains(&"save"), "the save and its backups stay through an update");
    }

    #[test]
    fn a_macos_package_replaces_the_install_root_in_place() {
        // what make-mac.sh zips: PokopiaRecomp-<version>/ with the launcher, tools/ and the runtime library at its root
        let tmp = std::env::temp_dir().join(format!("nxrt-apply-macos-{}", std::process::id()));
        let (install, staged, work) = (tmp.join("Contents").join("MacOS"), tmp.join("staged"), tmp.join("update"));
        let package = staged.join("PokopiaRecomp-0.11.0");
        fs::create_dir_all(package.join("tools").join("build")).unwrap();
        fs::create_dir_all(install.join("tools").join("build")).unwrap();
        fs::create_dir_all(&work).unwrap();
        fs::write(package.join("PokopiaLauncher"), b"new launcher").unwrap();
        fs::write(package.join("tools/build/pkp_build.py"), b"new tools").unwrap();
        let launcher = install.join("PokopiaLauncher");
        fs::write(&launcher, b"old launcher").unwrap();
        fs::write(install.join("tools/build/pkp_build.py"), b"old tools").unwrap();
        let root = crate::update::find_package(&staged, "0.11.0", "PokopiaLauncher").unwrap();
        let job = Job { root: &install, package: &root, work: &work, launcher_name: "PokopiaLauncher", launcher: &launcher, game_dll: None };
        apply(&job, &mut |_, _, _| {}).unwrap();
        assert_eq!(fs::read(&launcher).unwrap(), b"new launcher", "the launcher is replaced, not left beside a new tools/");
        assert_eq!(fs::read(install.join("tools/build/pkp_build.py")).unwrap(), b"new tools");
        let _ = fs::remove_dir_all(&tmp);
    }
}
