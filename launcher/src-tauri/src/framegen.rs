// SPDX-License-Identifier: GPL-3.0-or-later
//! AMD FSR 3.1 frame generation needs amd_fidelityfx_framegeneration_vk.dll, which the release does not carry: the settings page asks whether it is
//! there before it lets frame generation be switched on, and offers to download it (tools/build/fetch_dlss.py --file framegen: AMD's signed build
//! from the FidelityFX SDK v2.4.0, checked against a pinned sha256) into `<game>/upscalers/fsr3`, next to the loader.

use std::path::{Path, PathBuf};

use crate::dlss::{self, Progress};

pub const DLL: &str = "amd_fidelityfx_framegeneration_vk.dll";

pub fn target_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("upscalers").join("fsr3")
}

/// The DLL is in `<game>/upscalers/fsr3`.
pub fn present(game_dir: &Path) -> bool {
    target_dir(game_dir).join(DLL).is_file()
}

/// Downloads the DLL, with the progress of the DLSS download (the script and its output are the same).
pub fn fetch(game_dir: &Path, tools_dir: &Path, report: impl FnMut(Progress)) -> Result<(), String> {
    dlss::fetch_file("framegen", &target_dir(game_dir), tools_dir, report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn present_follows_the_file() {
        let dir = std::env::temp_dir().join(format!("framegen-test-{}", std::process::id()));
        let fsr3 = dir.join("upscalers").join("fsr3");
        std::fs::create_dir_all(&fsr3).unwrap();
        assert!(!present(&dir));
        std::fs::write(fsr3.join(DLL), b"").unwrap();
        assert!(present(&dir));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
