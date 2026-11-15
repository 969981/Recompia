// SPDX-License-Identifier: GPL-3.0-or-later
//! The play time of each profile's save, as the game counts it. While the game is in play the runtime copies the save's own count, once
//! a minute, to <save dir>/play_time.ini, one line per save: [play_time] <account folder name> = whole seconds
//! (runtime/games/pkp/core/pkp_play_time.cpp). A save nobody has played since that build has no line, so its profile has no time yet.

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use crate::ini::Ini;
use crate::profiles;
use crate::saveimport;

/// The file the runtime writes in the save folder (pkp_play_time.cpp kFileName).
pub const FILE: &str = "play_time.ini";
const SECTION: &str = "play_time";

/// Seconds played, by profile id, for the profiles whose save has a line. Each profile plays the account folder of its uid
/// (profiles.rs apply_active writes it as [account] uid), the built-in user's when it has none.
pub fn of_profiles(game_dir: &Path) -> HashMap<String, u64> {
    let (save_dir, _) = saveimport::locate(game_dir);
    let Ok(bytes) = fs::read(save_dir.join(FILE)) else { return HashMap::new() };
    let ini = Ini::parse(&String::from_utf8_lossy(&bytes));
    profiles::load(game_dir)
        .profiles
        .into_iter()
        .filter_map(|p| {
            let secs = ini.get(SECTION, &saveimport::account_key_of(&p.uid))?.trim().parse::<u64>().ok()?;
            Some((p.id, secs))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-playtime-{}-{}", tag, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join("save")).unwrap();
        d
    }

    #[test]
    fn each_profile_reads_its_saves_line() {
        let d = tmp("lines");
        // the first profile has no uid (the runtime's built-in user), the second its own
        fs::write(
            d.join(profiles::FILE),
            r#"{"active":"p2","profiles":[{"id":"default","name":"A","uid":"","nickname":"A","net_uid":""},{"id":"p2","name":"B","uid":"00112233445566778899aabbccddeeff","nickname":"B","net_uid":""},{"id":"p3","name":"C","uid":"0f0e0d0c0b0a09080706050403020100","nickname":"C","net_uid":""}]}"#,
        )
        .unwrap();
        fs::write(d.join("save").join(FILE), "[play_time]\r\n010000000000000070636552484e4341 = 13200\r\n7766554433221100ffeeddccbbaa9988 = 42\r\n").unwrap();
        let t = of_profiles(&d);
        assert_eq!(t.get("default"), Some(&13200));
        assert_eq!(t.get("p2"), Some(&42));
        assert_eq!(t.get("p3"), None, "a save without a line has no time");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn no_file_or_a_bad_value_is_no_time() {
        let d = tmp("none");
        assert!(of_profiles(&d).is_empty());
        fs::write(d.join("save").join(FILE), "[play_time]\n010000000000000070636552484e4341 = soon\n").unwrap();
        assert!(of_profiles(&d).is_empty());
        let _ = fs::remove_dir_all(&d);
    }
}
