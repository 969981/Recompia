// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's old file name after the release files were renamed: PokopiaLauncher.exe, shipped for good beside Recompia.exe.
//! Shortcuts made by hand, a Steam shortcut not yet pointed at the new file (rename_migrate.rs) and `PokopiaLauncher.exe --play`
//! entries start this; it starts Recompia.exe beside it with the same arguments and working folder, waits for it and exits with its
//! exit code, so Steam counts the game as running for as long as it runs. Linux and macOS have the same as a shell script
//! (old-name.sh).
#![cfg_attr(windows, windows_subsystem = "windows")]

use std::process::Command;

const LAUNCHER: &str = "Recompia.exe";

fn main() {
    std::process::exit(run());
}

fn run() -> i32 {
    #[cfg(windows)]
    console::attach_parents();
    let Some(target) = std::env::current_exe().ok().map(|me| me.with_file_name(LAUNCHER)) else { return 1 };
    let mut child = match Command::new(&target).args(std::env::args_os().skip(1)).spawn() {
        Ok(child) => child,
        Err(e) => {
            #[cfg(windows)]
            console::tell(&format!("{} could not be started ({e}). Download the latest release from https://github.com/Dittobyte/Recompia/releases and unpack it over this folder.", target.display()));
            #[cfg(not(windows))]
            eprintln!("{} could not be started: {e}", target.display());
            return 127;
        }
    };
    // after the start: a child inherits an ignored Ctrl+C, and the launcher's `--host` stops cleanly on it
    #[cfg(windows)]
    console::ignore_ctrl_c();
    child.wait().ok().and_then(|status| status.code()).unwrap_or(1)
}

#[cfg(windows)]
mod console {
    const ATTACH_PARENT_PROCESS: u32 = u32::MAX;
    #[link(name = "kernel32")]
    extern "system" {
        fn AttachConsole(pid: u32) -> i32;
        fn SetConsoleCtrlHandler(handler: Option<unsafe extern "system" fn(u32) -> i32>, add: i32) -> i32;
    }
    #[link(name = "user32")]
    extern "system" {
        fn MessageBoxW(wnd: isize, text: *const u16, caption: *const u16, kind: u32) -> i32;
    }

    /// A terminal that started this keeps its output: the launcher's own `--host` and `--play` attach to their parent's console, which
    /// is this one once it is attached. Fails harmlessly when nothing started this from a terminal.
    pub fn attach_parents() {
        unsafe {
            AttachConsole(ATTACH_PARENT_PROCESS);
        }
    }

    /// Ctrl+C is the launcher's to handle; this exits when the launcher does.
    pub fn ignore_ctrl_c() {
        unsafe {
            SetConsoleCtrlHandler(None, 1);
        }
    }

    /// A double click has no terminal to read: the reason goes into a message box.
    pub fn tell(text: &str) {
        let wide = |s: &str| s.encode_utf16().chain(std::iter::once(0)).collect::<Vec<u16>>();
        let (text, caption) = (wide(text), wide("Recompia"));
        const MB_ICONERROR: u32 = 0x10;
        unsafe {
            MessageBoxW(0, text.as_ptr(), caption.as_ptr(), MB_ICONERROR);
        }
    }
}
