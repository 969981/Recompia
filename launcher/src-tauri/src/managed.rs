// SPDX-License-Identifier: GPL-3.0-or-later
//! An install another launcher keeps. RecompLauncher installs a release from GitHub, puts every later release over it itself and
//! starts this launcher with RECOMPLAUNCHER=1 in its environment (its docs/DEVELOPERS.md: "Never update yourself when
//! RECOMPLAUNCHER=1"). Such an install takes no update of its own (update_cmds.rs update_version: two updaters on one folder undo
//! each other's files) and is its own game folder (paths.rs game_dir_of).

use std::ffi::OsStr;

/// This launcher was started by RecompLauncher, which keeps the install.
pub fn by_recomplauncher() -> bool {
    is_on(std::env::var_os("RECOMPLAUNCHER").as_deref())
}

fn is_on(value: Option<&OsStr>) -> bool {
    value.is_some_and(|v| v == "1")
}

#[cfg(test)]
mod tests {
    use super::is_on;
    use std::ffi::OsStr;

    #[test]
    fn only_one_means_kept_by_recomplauncher() {
        assert!(is_on(Some(OsStr::new("1"))));
        assert!(!is_on(None));
        assert!(!is_on(Some(OsStr::new(""))));
        assert!(!is_on(Some(OsStr::new("0"))));
    }
}
