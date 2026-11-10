// SPDX-License-Identifier: GPL-3.0-or-later
//! File helpers several modules share.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

/// UTC now as `yyyymmdd-hhmmss`: the part of a backup or damaged-copy folder or file name that says when it was made.
pub fn file_stamp() -> String {
    chrono::Utc::now().format("%Y%m%d-%H%M%S").to_string()
}

/// A temporary file name beside `path` that no other call gets: process id and a counter.
pub fn temp_beside(path: &Path) -> PathBuf {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    PathBuf::from(format!("{}.{}-{}.tmp", path.display(), std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)))
}

/// Which half of `write_atomic` failed. Callers word their own message from it: the temporary file's path is only worth showing for a
/// failed write, the target's for a failed replace.
pub enum AtomicFail {
    Write { tmp: PathBuf, error: io::Error },
    Replace { error: io::Error },
}

/// Writes `data` to a temporary file beside `path` (a name unique to this call, so two writers of one file never share it), then
/// renames it over `path`: the later rename wins, each with a whole file. A failed rename removes the temporary file.
pub fn write_atomic(path: &Path, data: &[u8]) -> Result<(), AtomicFail> {
    let tmp = temp_beside(path);
    fs::write(&tmp, data).map_err(|error| AtomicFail::Write { tmp: tmp.clone(), error })?;
    fs::rename(&tmp, path).map_err(|error| {
        let _ = fs::remove_file(&tmp);
        AtomicFail::Replace { error }
    })
}

/// Copies a folder and everything in it to `dst` (created). A file that cannot be copied stops the copy with its error.
pub fn copy_tree(src: &Path, dst: &Path) -> io::Result<()> {
    fs::create_dir_all(dst)?;
    for e in fs::read_dir(src)? {
        let e = e?;
        let (from, to) = (e.path(), dst.join(e.file_name()));
        if e.metadata()?.is_dir() {
            copy_tree(&from, &to)?;
        } else {
            fs::copy(&from, &to)?;
        }
    }
    Ok(())
}
