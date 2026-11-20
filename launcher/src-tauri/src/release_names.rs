// SPDX-License-Identifier: GPL-3.0-or-later
//! The file names of a release, in both naming schemes while the release files are renamed (two releases: first the updater knows
//! both, then releases switch): the asset `<prefix>-<version><suffix>` (releases.rs), the top folder `<prefix>-<version>/` it
//! unpacks to, and the launcher file at that folder's root, which is also its name in an install (update.rs, update_apply.rs).
//! Every list is new scheme first: a release that carries both installs from its new-scheme files. The new scheme also has its own
//! platform tokens (`-windows-x64.zip`, ...), which a launcher from before the rename cannot match by suffix, so which asset it
//! takes never depends on the order GitHub lists them in. Everything outside these names is the same in both schemes (pkp.exe,
//! pkp.ini, the save, the launcher's data folder).

use std::path::{Path, PathBuf};

/// The asset and top folder prefix, new first.
const PREFIXES: [&str; 2] = ["Recompia", "PokopiaRecomp"];

/// Whose launcher names: a parameter rather than cfg alone, so every host's tests run everywhere.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Host {
    Windows,
    Linux,
    MacOs,
}

impl Host {
    pub const HERE: Host = if cfg!(windows) {
        Host::Windows
    } else if cfg!(target_os = "macos") {
        Host::MacOs
    } else {
        Host::Linux
    };

    /// The launcher file at the package root, new first. macOS ships a plain binary in a flat folder (make-mac.sh), no `.app`.
    pub const fn launchers(self) -> [&'static str; 2] {
        match self {
            Host::Windows => ["Recompia.exe", "PokopiaLauncher.exe"],
            Host::Linux => ["Recompia.AppImage", "PokopiaLauncher.AppImage"],
            Host::MacOs => ["Recompia", "PokopiaLauncher"],
        }
    }
}

/// The platform's asset suffix in each scheme. `old` is what every launcher before the rename takes by suffix alone, whatever the
/// prefix: macOS releases so far are `Recompia-<v>-macos-arm64.zip`, an old-token name that stays the old scheme.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Suffixes {
    pub new: &'static str,
    pub old: &'static str,
}

impl Suffixes {
    pub const WINDOWS: Suffixes = Suffixes { new: "-windows-x64.zip", old: "-win64.zip" };
    pub const LINUX_X64: Suffixes = Suffixes { new: "-linux-x86_64.tar.gz", old: "-linux-x64.tar.gz" };
    pub const LINUX_ARM64: Suffixes = Suffixes { new: "-linux-aarch64.tar.gz", old: "-linux-arm64.tar.gz" };
    pub const MACOS: Suffixes = Suffixes { new: "-macos-aarch64.zip", old: "-macos-arm64.zip" };

    // The macOS zip (launcher/scripts/make-mac.sh) is a flat folder with the launcher binary at its root, no .app; that folder is
    // the install root the update is applied to.
    pub const HERE: Suffixes = if cfg!(windows) {
        Suffixes::WINDOWS
    } else if cfg!(target_os = "macos") {
        Suffixes::MACOS
    } else if cfg!(target_arch = "aarch64") {
        Suffixes::LINUX_ARM64
    } else {
        Suffixes::LINUX_X64
    };

    /// An asset of the new scheme: `Recompia-<v>` plus the new suffix.
    pub fn is_new(self, name: &str) -> bool {
        name.strip_prefix(PREFIXES[0]).is_some_and(|rest| rest.starts_with('-')) && name.ends_with(self.new)
    }

    /// An asset of the old scheme: any name ending in the old suffix (the first of them wins), whatever its prefix.
    pub fn is_old(self, name: &str) -> bool {
        name.ends_with(self.old)
    }
}

/// The unpacked package in `staged`: `<prefix>-<version>/` holding a launcher file and tools/build, any prefix with any launcher name,
/// new first. Returns the folder and the launcher's name in it. Asked before the install is touched.
pub fn find_package(staged: &Path, version: &str, host: Host) -> Result<(PathBuf, &'static str), String> {
    // Every host's archive puts the same tree under its top folder: the launcher, tools/ and the runtime library. That folder is
    // the install root, and an update replaces what is in it in place. A new-scheme package may also carry a file of the old
    // launcher name (a stub that starts the new one), so the new name is looked for first.
    for prefix in PREFIXES {
        let root = staged.join(format!("{prefix}-{version}"));
        if !root.join("tools").join("build").is_dir() {
            continue;
        }
        if let Some(name) = host.launchers().into_iter().find(|n| root.join(n).is_file()) {
            return Ok((root, name));
        }
    }
    Err("The downloaded package is not laid out as expected. Nothing was changed.".into())
}

/// Where the package's launcher `name` goes when `running` is the launcher file now: under its own name in `root` when the running file
/// is ours under either scheme's name (an update across the rename), else onto the running file, so a name the player gave it stays.
/// An install from an older macOS `.app` (`Contents/MacOS`, paths.rs) keeps its name too: the bundle's Info.plist names the binary.
pub fn landing(root: &Path, running: &Path, name: &str, host: Host) -> PathBuf {
    let bundle = root.ends_with("Contents/MacOS");
    // any case: Windows and macOS file names are case-insensitive, and a file Explorer shows as ours is ours
    let named = |n: &str| host.launchers().iter().any(|l| l.eq_ignore_ascii_case(n));
    let ours = !bundle && running.parent() == Some(root) && running.file_name().and_then(|n| n.to_str()).is_some_and(named);
    if ours {
        root.join(name)
    } else {
        running.to_path_buf()
    }
}

/// The launcher files of both schemes in `root`: an update across the rename leaves the other name's `.old` there.
pub fn launcher_files(root: &Path, host: Host) -> Vec<PathBuf> {
    host.launchers().iter().map(|n| root.join(n)).collect()
}

/// Which of the two names `launcher`'s file carries (any case): Some(true) the new one, Some(false) the old one, None neither (a name
/// the player gave it).
pub fn is_new_name(launcher: &Path, host: Host) -> Option<bool> {
    let name = launcher.file_name()?.to_str()?;
    host.launchers().iter().position(|l| l.eq_ignore_ascii_case(name)).map(|i| i == 0)
}

/// The other scheme's launcher beside `launcher` when it carries one of the two names: what a shortcut made before or after the
/// rename starts instead. None for a name the player gave it.
pub fn other_name(launcher: &Path, host: Host) -> Option<PathBuf> {
    let new = is_new_name(launcher, host)?;
    Some(launcher.with_file_name(host.launchers()[usize::from(new)]))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn each_platform_has_a_new_and_an_old_name_that_never_match_each_others_suffix() {
        for (sx, new, old) in [
            (Suffixes::WINDOWS, "Recompia-1.0.0-windows-x64.zip", "PokopiaRecomp-1.0.0-win64.zip"),
            (Suffixes::LINUX_X64, "Recompia-1.0.0-linux-x86_64.tar.gz", "PokopiaRecomp-1.0.0-linux-x64.tar.gz"),
            (Suffixes::LINUX_ARM64, "Recompia-1.0.0-linux-aarch64.tar.gz", "PokopiaRecomp-1.0.0-linux-arm64.tar.gz"),
            (Suffixes::MACOS, "Recompia-1.0.0-macos-aarch64.zip", "PokopiaRecomp-1.0.0-macos-arm64.zip"),
        ] {
            assert!(sx.is_new(new) && !sx.is_old(new), "{new}: a launcher from before the rename cannot match it");
            assert!(sx.is_old(old) && !sx.is_new(old), "{old}");
            for bad in ["PokopiaRecomp-1.0.0", "RecompiaX-1.0.0", ""] {
                assert!(!sx.is_new(&format!("{bad}{}", sx.new)), "{bad}");
            }
        }
        // the macOS release so far: the new prefix around the old token stays the old scheme
        assert!(Suffixes::MACOS.is_old("Recompia-0.14.2-macos-arm64.zip") && !Suffixes::MACOS.is_new("Recompia-0.14.2-macos-arm64.zip"));
        assert!(Suffixes::WINDOWS.is_old("Recompia-1.0.0-win64.zip") && !Suffixes::WINDOWS.is_new("Recompia-1.0.0-win64.zip"));
    }

    /// A staged folder with the given top folders, each with tools/build and the given files at its root.
    fn staged(name: &str, layout: &[(&str, &[&str])]) -> PathBuf {
        let tmp = std::env::temp_dir().join(format!("nxrt-names-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&tmp);
        for (top, files) in layout {
            fs::create_dir_all(tmp.join(top).join("tools/build")).unwrap();
            for f in *files {
                fs::write(tmp.join(top).join(f), b"x").unwrap();
            }
        }
        tmp
    }

    #[test]
    fn an_old_scheme_package_is_found_as_before() {
        let tmp = staged("old", &[("PokopiaRecomp-0.15.0", &["PokopiaLauncher.exe"])]);
        assert_eq!(find_package(&tmp, "0.15.0", Host::Windows).unwrap(), (tmp.join("PokopiaRecomp-0.15.0"), "PokopiaLauncher.exe"));
        assert!(find_package(&tmp, "0.15.1", Host::Windows).is_err(), "another version's folder is not this package");
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_new_scheme_package_is_found_and_its_new_launcher_wins_over_a_stub() {
        for (host, [new, old]) in [Host::Windows, Host::Linux, Host::MacOs].map(|h| (h, h.launchers())) {
            let tmp = staged(&format!("new-{host:?}"), &[("Recompia-0.16.0", &[new, old])]);
            assert_eq!(find_package(&tmp, "0.16.0", host).unwrap(), (tmp.join("Recompia-0.16.0"), new));
            let _ = fs::remove_dir_all(&tmp);
        }
    }

    #[test]
    fn either_folder_with_either_launcher_is_accepted_new_first() {
        // the macOS release so far: a new-scheme asset name around an old folder and binary
        let tmp = staged("mixed", &[("PokopiaRecomp-0.14.2", &["PokopiaLauncher"])]);
        assert_eq!(find_package(&tmp, "0.14.2", Host::MacOs).unwrap().1, "PokopiaLauncher");
        let _ = fs::remove_dir_all(&tmp);
        let tmp = staged("both", &[("PokopiaRecomp-0.16.0", &["PokopiaLauncher"]), ("Recompia-0.16.0", &["PokopiaLauncher"])]);
        assert_eq!(find_package(&tmp, "0.16.0", Host::MacOs).unwrap().0, tmp.join("Recompia-0.16.0"));
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn a_package_without_a_launcher_or_tools_is_refused() {
        let tmp = staged("bad", &[("Recompia-0.16.0", &["README.txt"])]);
        fs::write(tmp.join("PokopiaRecomp-0.16.0"), b"a file, not a folder").unwrap();
        assert!(find_package(&tmp, "0.16.0", Host::Windows).is_err());
        let _ = fs::remove_dir_all(&tmp);
        let tmp = std::env::temp_dir().join(format!("nxrt-names-notools-{}", std::process::id()));
        fs::create_dir_all(tmp.join("Recompia-0.16.0")).unwrap();
        fs::write(tmp.join("Recompia-0.16.0/Recompia.exe"), b"x").unwrap();
        assert!(find_package(&tmp, "0.16.0", Host::Windows).is_err(), "a launcher without tools/build is not a package");
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn the_new_launcher_lands_under_its_own_name_unless_the_player_renamed_theirs() {
        let root = Path::new("/games/r");
        for host in [Host::Windows, Host::Linux, Host::MacOs] {
            let [new, old] = host.launchers();
            assert_eq!(landing(root, &root.join(old), old, host), root.join(old), "same scheme: in place, as always");
            assert_eq!(landing(root, &root.join(old), new, host), root.join(new), "across the rename: its own name");
            assert_eq!(landing(root, &root.join(new), old, host), root.join(old), "and back");
            let mine = root.join("My launcher");
            assert_eq!(landing(root, &mine, new, host), mine, "a player's own name stays");
            let elsewhere = Path::new("/other").join(old);
            assert_eq!(landing(root, &elsewhere, new, host), elsewhere, "a launcher outside the install is replaced where it is");
        }
        assert_eq!(landing(root, &root.join("pokopialauncher.exe"), "Recompia.exe", Host::Windows), root.join("Recompia.exe"), "any case");
        let bundle = Path::new("/Applications/R.app/Contents/MacOS");
        let [new, old] = Host::MacOs.launchers();
        assert_eq!(landing(bundle, &bundle.join(old), new, Host::MacOs), bundle.join(old), "a bundle keeps the binary its Info.plist names");
    }

    #[test]
    fn each_name_knows_the_other_and_a_players_own_name_has_none() {
        let root = Path::new("/games/r");
        for host in [Host::Windows, Host::Linux, Host::MacOs] {
            let [new, old] = host.launchers();
            assert_eq!(is_new_name(&root.join(new), host), Some(true));
            assert_eq!(is_new_name(&root.join(old), host), Some(false));
            assert_eq!(other_name(&root.join(new), host), Some(root.join(old)));
            assert_eq!(other_name(&root.join(old), host), Some(root.join(new)));
            assert_eq!(is_new_name(&root.join("My launcher"), host), None);
            assert_eq!(other_name(&root.join("My launcher"), host), None);
        }
        assert_eq!(is_new_name(Path::new("/t/debug/recompia.exe"), Host::Windows), Some(true), "any case");
    }
}
