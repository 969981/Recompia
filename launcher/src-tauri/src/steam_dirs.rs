// SPDX-License-Identifier: GPL-3.0-or-later
//! Where Steam is on this machine (its installs and their signed-in accounts) and whether it runs: what steam.rs reads and writes in.
use std::fs;
use std::path::{Path, PathBuf};

pub fn roots() -> Vec<PathBuf> {
    roots_in(std::env::var_os("HOME").map(PathBuf::from))
}

/// The Steam installs under `home`: Linux keeps them under `.local/share`, macOS in `Library/Application Support`, and the Flatpak
/// in `.var`. Split from `roots()` so a test can point it at a folder of its own.
fn roots_in(home: Option<PathBuf>) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    #[cfg(unix)]
    if let Some(home) = home {
        #[cfg(target_os = "macos")]
        v.push(home.join("Library/Application Support/Steam"));
        #[cfg(not(target_os = "macos"))]
        {
            v.push(home.join(".local/share/Steam"));
            v.push(home.join(".steam/steam"));
            v.push(home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"));
        }
    }
    #[cfg(not(unix))]
    let _ = home;
    #[cfg(windows)]
    {
        for var in ["ProgramFiles(x86)", "ProgramFiles"] {
            if let Some(p) = std::env::var_os(var) {
                v.push(PathBuf::from(p).join("Steam"));
            }
        }
        v.push(PathBuf::from(r"C:\Program Files (x86)\Steam"));
    }
    let mut seen: Vec<PathBuf> = Vec::new();
    let mut out = Vec::new();
    for p in v {
        if !p.join("userdata").is_dir() {
            continue;
        }
        let real = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
        if seen.contains(&real) {
            continue;
        }
        seen.push(real);
        out.push(p);
    }
    out
}

/// The account folders of a Steam install (the numeric ones, not the shared 0).
pub fn accounts(root: &Path) -> Vec<PathBuf> {
    let Ok(rd) = fs::read_dir(root.join("userdata")) else { return Vec::new() };
    let mut v: Vec<PathBuf> = rd
        .flatten()
        .filter(|e| e.path().is_dir())
        .filter(|e| {
            let n = e.file_name().to_string_lossy().into_owned();
            n != "0" && !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit())
        })
        .map(|e| e.path())
        .collect();
    v.sort();
    v
}

pub fn steam_running() -> bool {
    #[cfg(target_os = "linux")]
    {
        let Ok(rd) = fs::read_dir("/proc") else { return false };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.bytes().all(|b| b.is_ascii_digit()) {
                continue;
            }
            if let Ok(c) = fs::read_to_string(e.path().join("comm")) {
                if c.trim() == "steam" {
                    return true;
                }
            }
        }
        false
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        std::process::Command::new("tasklist")
            .args(["/FI", "IMAGENAME eq steam.exe", "/NH"])
            .creation_flags(0x0800_0000)
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_ascii_lowercase().contains("steam.exe"))
            .unwrap_or(false)
    }
    #[cfg(target_os = "macos")]
    {
        // Steam runs as steam_osx. Its own folder is checked too, because pgrep is not guaranteed on a Mac without the developer tools.
        if std::process::Command::new("pgrep").arg("-x").arg("steam_osx").output().map(|o| o.status.success()).unwrap_or(false) {
            return true;
        }
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join("Library/Application Support/Steam/logs/connection_log.txt"))
            .is_some_and(|log| fs::metadata(log).and_then(|m| m.modified()).map(|t| t.elapsed().map(|e| e.as_secs() < 120).unwrap_or(false)).unwrap_or(false))
    }
    #[cfg(not(any(target_os = "linux", windows, target_os = "macos")))]
    {
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // unix only: Windows finds Steam under Program Files, not under a home folder a test can point at
    #[cfg(unix)]
    #[test]
    fn roots_find_the_platform_install() {
        let base = std::env::temp_dir().join(format!("steam_roots_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        #[cfg(target_os = "macos")]
        let steam = base.join("Library/Application Support/Steam");
        #[cfg(not(target_os = "macos"))]
        let steam = base.join(".local/share/Steam");
        fs::create_dir_all(steam.join("userdata").join("123").join("config")).unwrap();
        let roots = roots_in(Some(base.clone()));
        assert_eq!(roots, vec![steam.clone()]);
        assert_eq!(accounts(&steam).len(), 1);
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn accounts_skip_the_shared_folder() {
        let root = std::env::temp_dir().join(format!("recompia-steam-dirs-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        for n in ["0", "93249718", "notanumber"] {
            fs::create_dir_all(root.join("userdata").join(n)).unwrap();
        }
        let a = accounts(&root);
        assert_eq!(a.len(), 1);
        assert!(a[0].ends_with("93249718"));
        let _ = fs::remove_dir_all(&root);
    }
}
