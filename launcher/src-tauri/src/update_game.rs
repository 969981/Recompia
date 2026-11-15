// SPDX-License-Identifier: GPL-3.0-or-later
//! The game code an update brings (release model b), as update.rs's `GameCode`: built before the swap with the downloaded package's
//! own build tools (`pkp_build.py --prepare` through gamebuild.rs, into `<game>/build/next`) while the installed game stays
//! playable, moved into place right after the swap (`--adopt`), and put back if the update is undone (`--revert`).
//! Skipped, so the new launcher's Set up builds after the restart as before: in a development tree (no tools/build), in a game
//! folder that was never built, and for a package whose script cannot prepare (an older release picked in Choose a version).
//! The steps and the build's output go to setup.log, as Set up's do.

use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::Arc;

use crate::config::Config;
use crate::gamebuild::{self, Built, Next};
use crate::{dump, host, mods, paths, preflight, setuplog};

pub struct GameStep {
    game_dir: PathBuf,
    /// this install builds its game and has built it: there is something to prepare
    enabled: bool,
    exefs: Result<PathBuf, String>,
    jobs: usize,
    idle: bool,
    log: gamebuild::LogFn,
    tag: String,
    /// prepare left a build for adopt (built now, or found current from an earlier update that stopped)
    prepared: Cell<bool>,
}

impl GameStep {
    /// The step for an update to the release `tag`, from the launcher's settings as they are now.
    pub fn new(cfg: &Config, tag: &str) -> GameStep {
        let game_dir = paths::game_dir_of(cfg);
        let tools = paths::exe_dir().join("tools");
        let enabled = gamebuild::is_package_b(&tools) && gamebuild::built_state(&game_dir, &tools) != Built::No;
        // the game files Set up builds from: Linux and macOS read the pkp.ini Set up wrote, Windows the chosen dump (setup_job.rs)
        let exefs = if host::native_game() {
            Ok(host::native_exefs(&game_dir))
        } else {
            dump::scan(&cfg.dump_dir).chosen.map(|c| PathBuf::from(c.exefs)).ok_or_else(|| "Choose usable game files first.".to_string())
        };
        let cpus = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(3);
        let slog = Arc::new(setuplog::SetupLog::open(&game_dir));
        let log: gamebuild::LogFn = Arc::new(move |t: &str| slog.line(t));
        GameStep { game_dir, enabled, exefs, jobs: preflight::jobs_for(cpus, cfg.gentle_build).1, idle: cfg.gentle_build, log, tag: tag.to_string(), prepared: Cell::new(false) }
    }

    fn tools(&self, tools_dir: &Path) -> Result<gamebuild::Tools, String> {
        gamebuild::find_tools(tools_dir, &self.game_dir)
    }
}

impl crate::update::GameCode for GameStep {
    fn prepare(&self, tools_dir: &Path, cancel: &AtomicBool, report: &mut dyn FnMut(&str, u64, u64)) -> Result<bool, String> {
        if !self.enabled {
            return Ok(true);
        }
        let failed = |e: String| format!("The game could not be prepared for the update: {e} Nothing was changed. The details are in setup.log.");
        let tools = self.tools(tools_dir).map_err(failed)?;
        match gamebuild::next_state(&self.game_dir, &tools) {
            Next::Unsupported => {
                (self.log)(&format!("=== Update to {}: its build tools cannot prepare the game, Set up builds it after the restart", self.tag));
                return Ok(true);
            }
            Next::Current => {
                (self.log)(&format!("=== Update to {}: the game is already prepared", self.tag));
                self.prepared.set(true);
                return Ok(true);
            }
            Next::Missing => {}
        }
        let exefs = self.exefs.clone().map_err(failed)?;
        // the build puts in the hook points of the mods as they are now (Modtopia's hooks.tsv), as Set up does
        mods::refresh_plan(&self.game_dir).map_err(failed)?;
        (self.log)(&format!("=== Update to {}: preparing the game code while the installed game stays playable", self.tag));
        let job = gamebuild::Job { game_dir: self.game_dir.clone(), exefs, jobs: self.jobs, idle: self.idle, log: Some(self.log.clone()), prepare: true };
        let built = gamebuild::run(&job, &tools, cancel, |t| report(&t.phase, t.done, t.total)).map_err(failed)?;
        (self.log)(if built { "=== Update: the game code is prepared" } else { "=== Update: preparing the game code was cancelled" });
        self.prepared.set(built);
        Ok(built)
    }

    fn adopt(&self, tools_dir: &Path) -> Result<(), String> {
        if !self.prepared.get() {
            return Ok(());
        }
        (self.log)("=== Update: putting the prepared game code in place");
        gamebuild::swap_builds(&self.game_dir, &self.tools(tools_dir)?, false, Some(&self.log))
    }

    fn revert(&self, tools_dir: &Path) -> Result<(), String> {
        if !self.prepared.get() {
            return Ok(());
        }
        (self.log)("=== Update undone: putting the earlier game code back");
        gamebuild::swap_builds(&self.game_dir, &self.tools(tools_dir)?, true, Some(&self.log))
    }

    fn finish(&self) {
        if self.prepared.get() {
            gamebuild::forget_previous(&self.game_dir);
        }
    }
}
