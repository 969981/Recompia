// SPDX-License-Identifier: GPL-3.0-or-later
//! File helpers several modules share.

use std::fs;
use std::io;
use std::path::Path;

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
