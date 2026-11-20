// SPDX-License-Identifier: GPL-3.0-or-later
//! What the game's log says about a co-op start that found nobody to play with: the co-op module (runtime/third_party/pokopia-coop)
//! hides a host whose game build or mods fingerprint differs, and logs why. The runtime prefixes each of its lines with "coop: "
//! (ldn_native.cpp coop_hook_log). The page reads this after a co-op start it made, so a hidden host is said in plain words.
//!
//! The lines read (coop_mpnet.c peer_compat, coop_ldn.c):
//!   mp: host <tag> hidden from scans: its game build differs (...)
//!   mp: host <tag> hidden from scans: its mods or cheats differs (...)
//!   ldn: Connect: that host runs another build or other mods
//!   compat: gameplay mods active (<names>): fingerprint <hex>; only players with the same mods can join
//!
//! A co-op launch puts its co-op rules first in that list ("Recompia co-op rules <n>", runtime pkp_coop.h kCoopRules), so a build that
//! plays a session by other rules is refused like other mods. That entry is not a mod the player chose: it is left out of the mods
//! shown. A host runs another version of Recompia when its fingerprint is 0 (no rules entry and no mods: a build from before the rules
//! entry) while this one has the entry, or when it is this PC's own list with another rules number in front (the release changed the
//! rules: same mods, other version). The fingerprint is the coop module's coop_mods_add over the list (coop_interact.c: FNV-1a 64 of
//! each name, then 0xFF), so the launcher can make those candidates itself.

use serde::Serialize;
use std::path::Path;

#[derive(Serialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Mismatch {
    /// The host runs another Recompia build.
    Build,
    /// The host has other gameplay mods (or Link Play's Interact everywhere) on.
    Mods,
}

#[derive(Serialize, Clone, Debug, PartialEq, Eq)]
pub struct Finding {
    pub mismatch: Mismatch,
    /// The gameplay mods this PC had on, as the log names them; empty when none or not logged.
    pub own_mods: String,
}

/// The co-op rules entry of the mods list (runtime pkp_coop.h kCoopRules), with any number after it.
const RULES: &str = "Recompia co-op rules";

/// The rules numbers tried when a host's fingerprint is matched against this PC's list with other rules (kCoopRules counts up by one).
const RULES_MAX: u32 = 100;

/// The co-op mods fingerprint of a list, as the coop module makes it (coop_interact.c coop_mods_add, ldn_native.cpp coop_hook_mods).
fn fingerprint<'a>(names: impl IntoIterator<Item = &'a str>) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for name in names {
        for b in name.bytes().chain(std::iter::once(0xFFu8)) {
            h = (h ^ b as u64).wrapping_mul(0x100000001b3);
        }
    }
    h
}

/// The host plays by other co-op rules: its fingerprint is 0 (a build from before the rules entry), or this PC's own list (`own`, as the
/// compat line names it, rules entry first) with another rules number.
fn other_rules(own: &str, host: &str) -> bool {
    let names: Vec<&str> = own.split(", ").filter(|m| !m.is_empty()).collect();
    let Some((&own_rules, mods)) = names.split_first().filter(|(r, _)| r.starts_with(RULES)) else { return false };
    if host == "0000000000000000" {
        return true;
    }
    let Ok(host) = u64::from_str_radix(host, 16) else { return false };
    (0..RULES_MAX)
        .map(|k| format!("{RULES} {k}"))
        .filter(|rules| rules != own_rules)
        .any(|rules| fingerprint(std::iter::once(rules.as_str()).chain(mods.iter().copied())) == host)
}

/// The host's mods fingerprint as a "hidden from scans" line gives it: `(host <build>, mods <hex>; this one ...)`.
fn host_fingerprint(line: &str) -> Option<&str> {
    line.split("(host ").nth(1)?.split(", mods ").nth(1)?.split(';').next()
}

/// The mismatch the log names, if any. A host that matches again later in the same run clears it.
pub fn diagnose(log: &str) -> Option<Finding> {
    let mut found = None;
    let mut own_mods = String::new();
    for line in log.lines() {
        if line.contains("hidden from scans: its game build differs") {
            found = Some(Mismatch::Build);
        } else if line.contains("hidden from scans: its mods or cheats differs") {
            let other_version = host_fingerprint(line).is_some_and(|h| other_rules(&own_mods, h));
            found = if other_version { Some(Mismatch::Build) } else { found.or(Some(Mismatch::Mods)) };
        } else if line.contains("runs another build or other mods") {
            found = found.or(Some(Mismatch::Mods));
        } else if line.contains("matches this build again") {
            found = None;
        }
        if let Some(rest) = line.split("compat: gameplay mods active (").nth(1) {
            own_mods = rest.split("): fingerprint").next().unwrap_or("").to_string();
        } else if line.contains("compat: no gameplay mods active now") {
            own_mods.clear();
        }
    }
    let chosen: Vec<&str> = own_mods.split(", ").filter(|m| !m.is_empty() && !m.starts_with(RULES)).collect();
    let own_mods = chosen.join(", ");
    found.map(|mismatch| Finding { mismatch, own_mods })
}

/// The finding of the game's log in `game_dir` when it was written at or after `since` (unix seconds): only the run the page started.
pub fn last_run(game_dir: &Path, since: u64) -> Option<Finding> {
    let log = game_dir.join("pkp.log");
    let written = std::fs::metadata(&log).ok()?.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs();
    if written < since {
        return None;
    }
    diagnose(&String::from_utf8_lossy(&std::fs::read(&log).ok()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    const MODS: &str = "[I net] coop: compat: gameplay mods active (bigger_stacks@1.1.0): fingerprint 1a2b3c4d5e6f7081; only players with the same mods can join";

    #[test]
    fn a_host_with_other_mods_is_named() {
        let log = format!("{MODS}\n[I net] coop: mp: host the relay's room hidden from scans: its mods or cheats differs (host 0123456789abcdef, mods 0000000000000000; this one 0123456789abcdef, mods 1a2b3c4d5e6f7081)\n");
        assert_eq!(diagnose(&log), Some(Finding { mismatch: Mismatch::Mods, own_mods: "bigger_stacks@1.1.0".into() }));
    }

    #[test]
    fn another_build_wins_over_mods() {
        let log = "[I net] coop: ldn: Connect: that host runs another build or other mods\n[I net] coop: mp: host 192.0.2.7:47600 hidden from scans: its game build differs (host aaaa, mods 0; this one bbbb, mods 0)\n";
        assert_eq!(diagnose(log).unwrap().mismatch, Mismatch::Build);
    }

    const RULES_ONLY: &str = "[I net] coop: compat: gameplay mods active (Recompia co-op rules 2): fingerprint 0badc0ffee000001; only players with the same mods can join";

    #[test]
    fn the_rules_entry_is_not_shown_as_a_mod() {
        let log = format!("[I net] coop: compat: gameplay mods active (Recompia co-op rules 2, bigger_stacks@1.1.0): fingerprint 1a2b3c4d5e6f7081; only players with the same mods can join
[I net] coop: mp: host x hidden from scans: its mods or cheats differs (host 0123456789abcdef, mods 0000000000000000; this one 0123456789abcdef, mods 1a2b3c4d5e6f7081)
");
        assert_eq!(diagnose(&log), Some(Finding { mismatch: Mismatch::Build, own_mods: "bigger_stacks@1.1.0".into() }));
    }

    #[test]
    fn a_host_from_before_the_rules_entry_runs_another_version() {
        let log = format!("{RULES_ONLY}
[I net] coop: mp: host x hidden from scans: its mods or cheats differs (host 0123456789abcdef, mods 0000000000000000; this one 0123456789abcdef, mods 0badc0ffee000001)
");
        assert_eq!(diagnose(&log), Some(Finding { mismatch: Mismatch::Build, own_mods: String::new() }));
    }

    #[test]
    fn a_host_with_mods_and_the_rules_is_still_a_mods_mismatch() {
        let log = format!("{RULES_ONLY}
[I net] coop: mp: host x hidden from scans: its mods or cheats differs (host 0123456789abcdef, mods 77aa77aa77aa77aa; this one 0123456789abcdef, mods 0badc0ffee000001)
");
        assert_eq!(diagnose(&log), Some(Finding { mismatch: Mismatch::Mods, own_mods: String::new() }));
    }

    #[test]
    fn the_fingerprint_is_the_coop_modules() {
        // pkp.log of a report: "compat: gameplay mods active (Recompia co-op rules 2): fingerprint c5d983f4eddf1f4b"
        assert_eq!(fingerprint(["Recompia co-op rules 2"]), 0xc5d983f4eddf1f4b);
        assert_eq!(fingerprint(["Recompia co-op rules 3"]), 0xc5dd83f4ede30822);
    }

    #[test]
    fn a_host_on_other_rules_runs_another_version() {
        // A report: the visitor on 0.16.0 (rules 2), the host on a co-op test build (rules 3), the same mods on both
        let log = format!("{RULES_ONLY}
[I net] coop: mp: host [ip]:47610 (the relay's room) hidden from scans: its mods or cheats differs (host 76a489f3bd3642fa, mods c5dd83f4ede30822; this one 76a489f3bd3642fa, mods c5d983f4eddf1f4b)
");
        assert_eq!(diagnose(&log).unwrap().mismatch, Mismatch::Build);
        let own = "Recompia co-op rules 2, bigger_stacks@1.1.0";
        let host = format!("{:016x}", fingerprint(["Recompia co-op rules 3", "bigger_stacks@1.1.0"]));
        let log = format!("[I net] coop: compat: gameplay mods active ({own}): fingerprint {:016x}; only players with the same mods can join
[I net] coop: mp: host x hidden from scans: its mods or cheats differs (host 0123456789abcdef, mods {host}; this one 0123456789abcdef, mods 0)
", fingerprint(own.split(", ")));
        assert_eq!(diagnose(&log), Some(Finding { mismatch: Mismatch::Build, own_mods: "bigger_stacks@1.1.0".into() }));
    }

    #[test]
    fn a_clean_run_or_a_host_that_matches_again_says_nothing() {
        assert_eq!(diagnose("[I net] coop: mp: hosting through the relay 192.0.2.10:47610\n"), None);
        let log = "[I net] coop: mp: host x hidden from scans: its mods or cheats differs (h)\n[I net] coop: mp: host x matches this build again\n";
        assert_eq!(diagnose(log), None);
    }
}
