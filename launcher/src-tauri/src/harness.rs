//! Running the shader translator (tools/shaders/harness, `spirv_harness.exe`) for `shaders`: a run, and why a failed run failed.
use std::fs;
use std::path::Path;
use std::process::Command;
use std::sync::atomic::AtomicBool;

use crate::shaders::{run_tool, LogFn};

/// What the harness writes in its `--out` folder when a run fails (`ErrorFile` in tools/shaders/harness/Program.cs).
const HARNESS_ERROR: &str = "harness_error.txt";

/// `run_tool` for the shader translator writing to `stage`. Under Proton its stdout and stderr never reach the launcher (#366), so
/// a failure is explained from `stage`: the harness's own error file, or, with no `stage` folder at all, that it never started.
pub fn run_harness(cmd: Command, stage: &Path, cancel: &AtomicBool, idle: bool, log: Option<&LogFn>, line: impl FnMut(&str)) -> Result<bool, String> {
    run_tool(cmd, cancel, idle, log, None, line).map_err(|e| {
        let why = match fs::read_to_string(stage.join(HARNESS_ERROR)) {
            Ok(t) => format!("The shader translator reported:\n{}", t.trim()),
            Err(_) if !stage.is_dir() => {
                "The shader translator never started (it creates its output folder first): the program that runs it (Proton or Wine) failed before it."
                    .to_string()
            }
            Err(_) => "The shader translator started but wrote no error report.".to_string(),
        };
        if let Some(f) = log {
            f(&why);
        }
        format!("{}\n{}", e, why)
    })
}
