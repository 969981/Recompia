// SPDX-License-Identifier: GPL-3.0-or-later
//! The button icon pack (assets/prompts/prompt_icons.pak, built by runtime/scripts/build_prompt_pack.py) for the Settings page's
//! preview of the Button icons choice. The page parses it (src/prompt-pack-model.js); here it is only found and read.

use std::path::{Path, PathBuf};

use crate::paths::exe_dir;

const PACK: &str = "assets/prompts/prompt_icons.pak";

/// The pack of the game folder (where the release puts it), else the one next to the launcher.
fn pack_path(game_dir: &Path) -> Option<PathBuf> {
    [game_dir.join(PACK), exe_dir().join(PACK)].into_iter().find(|p| p.is_file())
}

/// The pack's bytes, or why there are none.
pub fn read_pack(game_dir: &Path) -> Result<Vec<u8>, String> {
    let path = pack_path(game_dir).ok_or_else(|| format!("{PACK} not found in the game folder"))?;
    std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))
}
