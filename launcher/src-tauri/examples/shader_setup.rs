// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's shader setup without the window (development check):
//!   cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]
//! Builds <game dir>/shader_cache/spirv exactly as the launcher's Set up does; `limit` translates only that many blocks.
//! Native translation only, into a separate output folder (no game run):
//!   cargo run --example shader_setup -- --native <harness dll> <corpus> <out> [--gentle]

// host.rs reaches the launcher's own modules, so an example carries the same set the binary declares (main.rs), by path.
#[path = "../src/child_lines.rs"]
mod child_lines;
#[allow(dead_code, clippy::all)]
#[path = "../src/config.rs"]
mod config;
#[allow(dead_code)]
#[path = "../src/download.rs"]
mod download;
#[allow(dead_code, clippy::all)]
#[path = "../src/dump.rs"]
mod dump;
#[allow(dead_code, clippy::all)]
#[path = "../src/fsutil.rs"]
mod fsutil;
#[allow(dead_code)]
#[path = "../src/gamebuild.rs"]
mod gamebuild;
#[allow(dead_code)]
#[path = "../src/harness.rs"]
mod harness;
#[allow(dead_code)]
#[path = "../src/host.rs"]
mod host;
#[allow(dead_code, clippy::all)]
#[path = "../src/ini.rs"]
mod ini;
#[allow(dead_code, clippy::all)]
#[path = "../src/paths.rs"]
mod paths;
#[path = "../src/proc_tree.rs"]
mod proc_tree;
#[allow(dead_code)]
#[path = "../src/runtime_api.rs"]
mod runtime_api;
#[allow(dead_code, clippy::all)]
#[path = "../src/settings.rs"]
mod settings;
#[allow(dead_code)]
#[path = "../src/shader_archive.rs"]
mod shader_archive;
#[allow(dead_code)]
#[path = "../src/shaders.rs"]
mod shaders;

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.first().is_some_and(|arg| arg == "--native") && a.len() >= 4 {
        let output = PathBuf::from(&a[3]);
        std::fs::create_dir_all(&output).expect("create translation output folder");
        let log: shaders::LogFn = std::sync::Arc::new(|text| println!("{text}"));
        match harness::run_native(&PathBuf::from(&a[1]), &PathBuf::from(&a[2]), &output, &AtomicBool::new(false), a.iter().any(|a| a == "--gentle"), Some(&log), |line| println!("{line}")) {
            Ok(true) => return,
            result => {
                eprintln!("{}", result.err().unwrap_or_else(|| "cancelled".into()));
                std::process::exit(1);
            }
        }
    }
    if a.len() < 4 {
        eprintln!("usage: shader_setup <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]");
        std::process::exit(2);
    }
    let job = shaders::Job {
        game_dir: PathBuf::from(&a[0]),
        romfs: PathBuf::from(&a[1]),
        romfs_base: None,
        main: PathBuf::from(&a[2]),
        limit: a.get(4).and_then(|x| x.parse().ok()).unwrap_or(0),
        compact: std::env::var_os("PKP_LOOSE_SHADERS").is_none(),
        jobs: a.get(5).and_then(|x| x.parse().ok()).unwrap_or(2),
        idle: false,
        log: None,
    };
    let tools = shaders::find_tools(&PathBuf::from(&a[3])).unwrap_or_else(|e| {
        eprintln!("{}", e);
        std::process::exit(1)
    });
    let mut last = String::new();
    // PKP_CANCEL_AFTER=<seconds> presses Pause that long into the run: the tool is killed and what it finished is kept.
    static CANCEL: AtomicBool = AtomicBool::new(false);
    if let Some(s) = std::env::var_os("PKP_CANCEL_AFTER").and_then(|v| v.to_str().and_then(|v| v.parse::<u64>().ok())) {
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(s));
            CANCEL.store(true, std::sync::atomic::Ordering::SeqCst);
        });
    }
    let r = shaders::run(&job, &tools, &CANCEL, |p| {
        if p.message != last {
            println!("[{}] {}", p.phase, p.message);
            last = p.message;
        }
    });
    match r {
        Ok(_) => println!("ok"),
        Err(e) => {
            eprintln!("error: {}", e);
            std::process::exit(1)
        }
    }
}
