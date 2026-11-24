// SPDX-License-Identifier: GPL-3.0-or-later
//! The build tools a Linux or macOS launcher fetches for the native Set up, one pinned table per platform: Python with lz4 and
//! capstone on every POSIX host, Zig on Linux. Part of host.rs (`host::tools`, re-exported there); Windows bundles them.

use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use super::{command, python, zig};
use crate::download::{self, Pinned};

// python-build-standalone 20261013 (its SHA256SUMS), the PyPI wheels the Windows release bundles, Zig 0.17.0 (ziglang.org
// download index). Bump together with the Windows release's tools/python and tools/build/zig.
const PYTHON: Pinned<'static> = Pinned {
    file: "cpython-3.13.16+20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
    size: 35_099_487,
    sha256: "d0080766b6b74aeb81e1949ef0d9a20e6c17f7b160fb295a69b936ccb17660be",
};
const WHEELS: [Pinned<'static>; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
        url: "https://files.pythonhosted.org/packages/d2/fb/ba9256c48266a09012ed1d9b0253b9aa4fe9cdff094f8febf5b26a4aa2a2/lz4-4.4.5-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
        size: 1_368_257,
        sha256: "451039b609b9a88a934800b5fc6ee401c89ad9c175abf2f4d9f8b2e4ef1afc64",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
        url: "https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
        size: 1_485_316,
        sha256: "273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e",
    },
];
const PYTHON_LINUX_ARM64: Pinned<'static> = Pinned {
    file: "cpython-3.13.16+20261013-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz",
    size: 29_331_623,
    sha256: "69a30ed26198178732c37b8de0b6b5191479fbacfe526954da776befe38f18e7",
};
const WHEELS_LINUX_ARM64: [Pinned<'static>; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-manylinux2014_aarch64.manylinux_2_17_aarch64.manylinux_2_28_aarch64.whl",
        url: "https://files.pythonhosted.org/packages/89/d1/f1d259352227bb1c185288dd694121ea303e43404aa77560b879c90e7073/lz4-4.4.5-cp313-cp313-manylinux2014_aarch64.manylinux_2_17_aarch64.manylinux_2_28_aarch64.whl",
        size: 1_279_639,
        sha256: "533298d208b58b651662dd972f52d807d48915176e5b032fb4f8c3b6f5fe535c",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.whl",
        url: "https://files.pythonhosted.org/packages/45/d9/dbeadb9fcd461e608ce79b1cb647fed6baa812fa6e1c68b0ba5ef81291af/capstone-5.0.9-py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.whl",
        size: 1_461_016,
        sha256: "41772ad0c71aa5d3e97d541e802593c2e71fb94ac7e20d4202f97ce2a3eb8ed5",
    },
];
const PYTHON_MACOS: Pinned<'static> = Pinned {
    file: "cpython-3.13.16+20261013-aarch64-apple-darwin-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-aarch64-apple-darwin-install_only_stripped.tar.gz",
    size: 25_268_458,
    sha256: "16a5332fb0dcb77f163fc8f8cbccd95be1644cc53e68e5d2a79f50afd6680a0e",
};
const WHEELS_MACOS: [Pinned<'static>; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl",
        url: "https://files.pythonhosted.org/packages/8f/3f/ea3334e59de30871d773963997ecdba96c4584c5f8007fd83cfc8f1ee935/lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl",
        size: 207_163,
        sha256: "b424df1076e40d4e884cfcc4c77d815368b7fb9ebcd7e634f937725cd9a8a72a",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-macosx_11_0_arm64.whl",
        url: "https://files.pythonhosted.org/packages/42/9a/4eecfbc94961cda70301fd13de69042be1943bee456e37bc1369840601b1/capstone-5.0.9-py3-none-macosx_11_0_arm64.whl",
        size: 1_202_361,
        sha256: "fc269c5082e17d7f9f265cd6980e2fdc6ab572df913951d76db0357e06b8001d",
    },
];
const ZIG: Pinned<'static> = Pinned {
    file: "zig-x86_64-linux-0.17.0.tar.xz",
    url: "https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz",
    size: 57_332_648,
    sha256: "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026",
};
const ZIG_LINUX_ARM64: Pinned<'static> = Pinned {
    file: "zig-aarch64-linux-0.17.0.tar.xz",
    url: "https://ziglang.org/download/0.17.0/zig-aarch64-linux-0.17.0.tar.xz",
    size: 52_877_280,
    sha256: "9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8",
};

/// Where the fetched tools go: next to the AppImage on Linux (exe_dir is the .AppImage's folder), and in the game folder on
/// macOS, because the launcher runs from inside a bundle that must not be written to (a signature covers it, and
/// /Applications may be owned by another account).
pub fn tools_dir_for(game_dir: &Path) -> PathBuf {
    if cfg!(target_os = "macos") {
        game_dir.join("tools")
    } else {
        crate::paths::exe_dir().join("tools")
    }
}

/// The Linux Python can run the release's scripts.
fn python_ready(py: &Path) -> bool {
    let mut c = command(py);
    c.args(["-I", "-B", "-c", "import lz4.block, capstone"]).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    c.status().map(|s| s.success()).unwrap_or(false)
}

/// Linux and macOS: fetches the native build tools into `tools/linux` or `tools/macos` (python/ with lz4 and capstone, and
/// zig/ on Linux) unless they are there. `report(done, total, text)` gets the download progress. Ok(false) = cancelled.
/// Windows: nothing to do, the package bundles them.
pub fn prepare_tools(tools_dir: &Path, cancel: &AtomicBool, report: &mut dyn FnMut(u64, u64, String)) -> Result<bool, String> {
    if cfg!(windows) {
        return Ok(true);
    }
    let macos = cfg!(target_os = "macos");
    let arm64 = cfg!(target_arch = "aarch64");
    let what = if macos { "macOS" } else { "Linux" };
    let (python_pin, wheels): (&Pinned, &[Pinned]) = if macos {
        (&PYTHON_MACOS, &WHEELS_MACOS)
    } else if arm64 {
        (&PYTHON_LINUX_ARM64, &WHEELS_LINUX_ARM64)
    } else {
        (&PYTHON, &WHEELS)
    };
    let zig_pin: &Pinned = if arm64 { &ZIG_LINUX_ARM64 } else { &ZIG };
    let platform = tools_dir.join(if macos { "macos" } else { "linux" });
    let (py, zig) = (python(tools_dir), zig(tools_dir));
    let py_ok = python_ready(&py);
    let zig_ok = macos || zig.is_file();   // macOS compiles with Apple clang
    if py_ok && zig_ok {
        return Ok(true);
    }
    let dl = platform.join("downloads");
    std::fs::create_dir_all(&dl).map_err(|e| format!("cannot create {}: {}", dl.display(), e))?;
    let mut need: Vec<&Pinned> = Vec::new();
    if !py_ok {
        need.push(python_pin);
        need.extend(wheels.iter());
    }
    if !zig_ok {
        need.push(zig_pin);
    }
    let total: u64 = need.iter().map(|p| p.size).sum();
    let (agent, mut done) = (download::agent(true), 0u64);
    for p in &need {
        let mb = |b: u64| b as f64 / (1u64 << 20) as f64;
        let ok = download::fetch(&agent, p, &dl, cancel, &mut |n| {
            report(done + n, total, format!("Downloading the {} build tools: {:.0} of {:.0} MB", what, mb(done + n), mb(total)))
        })?;
        if !ok {
            return Ok(false);
        }
        done += p.size;
    }
    if !py_ok {
        report(total, total, format!("Installing the {} build tools: Python...", what));
        unpack(&dl.join(python_pin.file), &platform, "python")?;
        let mut cmd = command(&py);
        cmd.args(["-I", "-B", "-m", "pip", "install", "-q", "--no-index", "--no-deps", "--no-compile", "--disable-pip-version-check", "--no-warn-script-location"]);
        cmd.args(wheels.iter().map(|w| dl.join(w.file)));
        let out = cmd.output().map_err(|e| format!("could not start {}: {}", py.display(), e))?;
        if !out.status.success() || !python_ready(&py) {
            return Err(format!("Installing lz4 and capstone into {} failed: {}", py.display(), String::from_utf8_lossy(&out.stderr).trim()));
        }
    }
    if !zig_ok {
        report(total, total, format!("Installing the {} build tools: Zig...", what));
        unpack(&dl.join(zig_pin.file), &platform, "zig")?;
    }
    let _ = std::fs::remove_dir_all(&dl);
    Ok(true)
}

/// Unpacks a tarball with one top-level folder (python/, zig-x86_64-linux-0.17.0/) as `parent/name`, replacing an older one.
fn unpack(archive: &Path, parent: &Path, name: &str) -> Result<(), String> {
    let tmp = parent.join(format!(".unpack-{}", name));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| format!("cannot create {}: {}", tmp.display(), e))?;
    let out = command("tar").arg("-xf").arg(archive).arg("-C").arg(&tmp).output().map_err(|e| format!("could not start tar: {}", e))?;
    if !out.status.success() {
        return Err(format!("Unpacking {} failed: {}", archive.display(), String::from_utf8_lossy(&out.stderr).trim()));
    }
    let top: Vec<PathBuf> = std::fs::read_dir(&tmp).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).collect();
    let [only] = top.as_slice() else {
        return Err(format!("{} does not hold one folder", archive.display()));
    };
    let dst = parent.join(name);
    let _ = std::fs::remove_dir_all(&dst);
    std::fs::rename(only, &dst).map_err(|e| format!("cannot move {}: {}", dst.display(), e))?;
    let _ = std::fs::remove_dir_all(&tmp);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "downloads about 27 MB from python-build-standalone and PyPI"]
    fn prepare_tools_fetches_a_working_python() {
        let dir = std::env::temp_dir().join(format!("pkp_tools_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let cancel = std::sync::atomic::AtomicBool::new(false);
        let mut last = String::new();
        let ok = prepare_tools(&dir, &cancel, &mut |_, _, m| last = m).unwrap();
        assert!(ok);
        assert!(python_ready(&python(&dir)), "the fetched Python cannot import lz4 and capstone: {last}");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
