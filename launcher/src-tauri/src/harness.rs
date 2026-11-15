//! Running the shader translator (tools/shaders/harness, `spirv_harness.exe`) for `shaders`: a run, a worker that runs chunk after
//! chunk in one process, and why a failed run failed.
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::time::Duration;

use crate::shaders::{run_tool, LogFn};

#[path = "harness_resources.rs"]
mod resources;

/// What the harness writes in its `--out` folder when a run fails (`ErrorFile` in tools/shaders/harness/Program.cs).
const HARNESS_ERROR: &str = "harness_error.txt";

/// What the harness writes in its `--out` folder as a run goes (`ProgressFile` in tools/shaders/harness/Program.cs).
const HARNESS_PROGRESS: &str = "harness_progress.txt";
/// The translator jobs of a safe run.
const SAFE_JOBS: usize = 2;

/// How the translator runs. Fast: server GC (the harness's own default; with workstation GC each collection stops every worker,
/// about 4x slower) and every job. Safe: workstation GC and at most SAFE_JOBS jobs, for a translator that died under the fast
/// settings without a report (#530: exit status 2 about 3 s after start, every time, on a Steam Deck with 11.5 GB under Proton).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    Fast,
    Safe,
}

/// The translator over `input` (a corpus folder with manifest.tsv / unique.tsv) into `stage`.
fn command(exe: &Path, input: &Path, stage: &Path, jobs: usize, mode: Mode) -> Result<Command, String> {
    let jobs = if mode == Mode::Safe { jobs.clamp(1, SAFE_JOBS) } else { jobs.max(1) };
    // the package's own build for this platform, run as it is: spirv_harness.exe on Windows, the native apphost elsewhere
    let mut cmd = crate::host::command(exe);
    cmd.arg("--batch").arg(input).arg("--out").arg(stage);
    cmd.args(["--jobs", &jobs.to_string(), "--no-val"]);
    gc_env(&mut cmd, jobs, mode);
    Ok(cmd)
}

/// The translator's runtime settings for a translation `jobs` blocks wide.
///
/// A block's output is one ~263 KB byte[] and there are 99,012 of them, so at the default 85 KB large-object threshold all 26 GB of
/// SPIR-V is large-object allocation that only gen2 collects. A 1 MiB threshold puts those arrays in gen0, and a gen0 wide enough to
/// absorb them costs fewer collections still. Measured on the 2.0.1 corpus, 7,000 blocks at 23 jobs: 30 s before, 26.5 s with the
/// threshold, 24 s with both. Server GC is the harness's own default and is what makes 23 workers worth having; the safe run is a
/// diagnostic and keeps the runtime's own settings.
fn gc_env(cmd: &mut Command, jobs: usize, mode: Mode) {
    if mode != Mode::Fast {
        cmd.env("DOTNET_gcServer", "0");
        return;
    }
    cmd.env("DOTNET_gcServer", "1");
    cmd.env("DOTNET_GCLOHThreshold", "0x100000");
    // server GC gives every heap its own gen0, so this is 32 MB per translator thread, between 128 MB and 512 MB
    let mb = (jobs * 32).clamp(128, 512);
    cmd.env("DOTNET_GCgen0size", format!("0x{:x}", mb << 20));
}

/// A failed run: the message for setup.log, and whether the harness wrote an error report (an error it caught, not a process that
/// went without a word).
pub(crate) struct Failure {
    pub message: String,
    pub reported: bool,
    /// the report names a System.OutOfMemoryException: worth one retry with a smaller heap
    pub memory: bool,
}

/// Why a run failed, from what it leaves in `stage`: its own error file, or, with no `stage` folder at all, that it never started,
/// and the last steps of its progress file. (Under Proton its stdout and stderr never reach the launcher, #366.)
fn explain(stage: &Path, why: String) -> Failure {
    let report = fs::read_to_string(stage.join(HARNESS_ERROR)).ok();
    let mut text = match &report {
        Some(t) => format!("The shader translator reported:\n{}", t.trim()),
        None if !stage.is_dir() => {
            "The shader translator never started (it creates its output folder first): the program that runs it failed before it."
                .to_string()
        }
        None => "The shader translator started but wrote no error report.".to_string(),
    };
    if let Ok(p) = fs::read_to_string(stage.join(HARNESS_PROGRESS)) {
        let steps: Vec<&str> = p.lines().filter(|l| !l.trim().is_empty()).collect();
        text.push_str("\nIts last steps:");
        for l in &steps[steps.len().saturating_sub(4)..] {
            text.push_str("\n  ");
            text.push_str(l);
        }
    }
    let memory = report.as_ref().is_some_and(|t| t.contains("System.OutOfMemoryException"));
    Failure { message: format!("{}\n{}", why, text), reported: report.is_some(), memory }
}

/// `run_tool` for the shader translator writing to `stage`, with a failure explained from `stage`.
fn run_once(cmd: Command, stage: &Path, cancel: &AtomicBool, idle: bool, log: Option<&LogFn>, line: impl FnMut(&str)) -> Result<bool, Failure> {
    run_tool(cmd, cancel, idle, log, None, line).map_err(|e| {
        let f = explain(stage, e);
        if let Some(l) = log {
            l(&f.message);
        }
        f
    })
}

/// One translator run over `input` into `stage` (emptied first). A run that dies without a report under `Mode::Fast` is run again
/// once under `Mode::Safe`, and `mode` stays Safe for the caller's later runs. Ok(false): cancelled.
#[allow(clippy::too_many_arguments)]
pub fn run(
    exe: &Path,
    input: &Path,
    stage: &Path,
    jobs: usize,
    mode: &mut Mode,
    cancel: &AtomicBool,
    idle: bool,
    log: Option<&LogFn>,
    mut line: impl FnMut(&str),
) -> Result<bool, String> {
    let _ = fs::remove_dir_all(stage);
    match run_once(command(exe, input, stage, jobs, *mode)?, stage, cancel, idle, log, &mut line) {
        Err(f) if !f.reported && *mode == Mode::Fast => {
            *mode = Mode::Safe;
            if let Some(l) = log {
                l(&format!("The shader translator stopped without a report: running it again with workstation GC and {} jobs, and keeping that.", jobs.clamp(1, SAFE_JOBS)));
            }
            let _ = fs::remove_dir_all(stage);
            run_once(command(exe, input, stage, jobs, Mode::Safe)?, stage, cancel, idle, log, &mut line).map_err(|f| f.message)
        }
        r => r.map_err(|f| f.message),
    }
}

/// One translator process for a whole translation (`--serve` in tools/shaders/harness/Program.cs): the launcher names each chunk's
/// corpus folder on its stdin and waits for `chunk done`, so a chunk no longer pays the .NET start, the cold JIT and the thread pool
/// ramp (about 3 s of every chunk on a 24-thread PC, and every one of them on a Steam Deck).
pub struct Worker {
    child: Child,
    stdin: Option<ChildStdin>,
    lines: mpsc::Receiver<String>,
    stderr: mpsc::Receiver<String>,
    name: String,
    stage: PathBuf,
    /// the last lines it printed, for a failure message
    tail: Vec<String>,
}

impl Worker {
    /// Starts the translator over `stage` in `mode`.
    pub fn start(exe: &Path, stage: &Path, jobs: usize, mode: Mode, idle: bool) -> Result<Worker, String> {
        let jobs = if mode == Mode::Safe { jobs.clamp(1, SAFE_JOBS) } else { jobs.max(1) };
        // the package's own build for this platform, run as it is: spirv_harness.exe on Windows, the native apphost elsewhere
        let mut cmd = crate::host::command(exe);
        cmd.arg("--serve").arg("--out").arg(stage).args(["--jobs", &jobs.to_string(), "--no-val"]);
        gc_env(&mut cmd, jobs, mode);
        crate::shaders::quiet(&mut cmd, idle);
        let name = exe.to_string_lossy().to_string();
        let mut child = cmd
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| format!("could not start {}: {}", name, e))?;
        let out = child.stdout.take().unwrap();
        let (tx, lines) = mpsc::channel::<String>();
        std::thread::spawn(move || crate::child_lines::lines(out).for_each(|l| drop(tx.send(l))));
        let err = child.stderr.take().unwrap();
        let (etx, stderr) = mpsc::channel::<String>();
        std::thread::spawn(move || {
            let mut tail: Vec<String> = Vec::new();
            for l in crate::child_lines::lines(err) {
                tail.push(l);
                if tail.len() > 12 {
                    tail.remove(0);
                }
            }
            let _ = etx.send(tail.join("\n"));
        });
        let stdin = child.stdin.take();
        Ok(Worker { child, stdin, lines, stderr, name, stage: stage.to_path_buf(), tail: Vec::new() })
    }

    /// Translates one chunk: `input` is the corpus folder holding its unique.tsv. Ok(true) finished, Ok(false) paused, Err: the
    /// process is gone (the caller starts another one).
    pub fn run_chunk(&mut self, input: &Path, cancel: &AtomicBool, log: Option<&LogFn>, mut line: impl FnMut(&str)) -> Result<bool, Failure> {
        let sent = match self.stdin.as_mut() {
            Some(stdin) => writeln!(stdin, "{}", input.to_string_lossy()).and_then(|()| stdin.flush()),
            None => Err(std::io::Error::new(std::io::ErrorKind::BrokenPipe, "its input is closed")),
        };
        if let Err(e) = sent {
            return Err(explain(&self.stage, format!("cannot send a chunk to {}: {}", self.name, e)));
        }
        loop {
            match self.lines.recv_timeout(Duration::from_millis(200)) {
                Ok(l) => {
                    match l.trim() {
                        "chunk done" => return Ok(true),
                        "chunk failed" => return Err(explain(&self.stage, format!("{} reported a failure on this chunk", self.name))),
                        _ => {}
                    }
                    if !crate::shaders::is_progress_line(&l) {
                        if let Some(f) = log {
                            f(&format!("out: {}", l));
                        }
                    }
                    self.tail.push(l.clone());
                    if self.tail.len() > 12 {
                        self.tail.remove(0);
                    }
                    line(&l);
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    let status = self.child.wait().map(|s| s.to_string()).unwrap_or_else(|e| e.to_string());
                    let tail = self.stderr.recv_timeout(Duration::from_secs(5)).unwrap_or_default();
                    let why = format!("{} stopped ({}). Last output:\n{}\nLast error output:\n{}", self.name, status, self.tail.join("\n"), tail);
                    return Err(explain(&self.stage, why));
                }
            }
            if cancel.load(Ordering::SeqCst) {
                // at once, not after the chunk: a pause the player pressed has to stop now (what the chunk finished is already in the
                // stage, and that is what gets committed)
                self.kill();
                return Ok(false);
            }
        }
    }

    /// Ends the process at once.
    pub fn kill(&mut self) {
        self.stdin = None;
        crate::host::kill(&mut self.child);
        let _ = self.child.wait();
    }

    /// Ends the process: `quit` first, then a kill if it does not go.
    pub fn stop(&mut self) {
        if let Some(mut stdin) = self.stdin.take() {
            let _ = writeln!(stdin, "quit");
            let _ = stdin.flush();
        }
        let t = std::time::Instant::now();
        while t.elapsed() < Duration::from_secs(10) {
            if matches!(self.child.try_wait(), Ok(Some(_))) {
                return;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        crate::host::kill(&mut self.child);
        let _ = self.child.wait();
    }
}

/// Native setup and the post-game missing-shader pass use the same resource policy. Only diagnostics are removed between
/// attempts: `stage` can be the player's existing loose cache, whose completed shaders must survive cancellation/failure.
pub fn run_native(
    dll: &Path,
    input: &Path,
    stage: &Path,
    cancel: &AtomicBool,
    gentle: bool,
    log: Option<&LogFn>,
    line: impl FnMut(&str),
) -> Result<bool, String> {
    if cancel.load(std::sync::atomic::Ordering::SeqCst) { return Ok(false); }
    let settings = resources::Settings::detect(gentle)?;
    run_native_with(dll, input, stage, cancel, gentle, log, settings, || resources::Settings::detect(true), line)
}

fn native_command(dll: &Path, input: &Path, stage: &Path, settings: resources::Settings) -> Command {
    let apphost = dll.with_file_name("spirv_harness");
    let mut cmd = if apphost.is_file() { crate::host::command(&apphost) } else { crate::host::command("dotnet") };
    if !apphost.is_file() {
        cmd.arg(dll);
    }
    cmd.arg("--batch").arg(input).arg("--out").arg(stage)
        .args(["--jobs", &settings.jobs.to_string(), "--host", crate::host::harness_host(), "--no-val"])
        .env("DOTNET_gcServer", if settings.mode == Mode::Fast { "1" } else { "0" })
        .env("DOTNET_GCHeapHardLimit", format!("0x{:X}", settings.heap));
    cmd
}

/// Batch catches per-block exceptions and still exits 0. An OOM recorded in results.tsv must take the same retry/failure
/// path as a process-level OOM, rather than letting native setup mark a memory-starved batch complete.
fn check_native_results(stage: &Path) -> Result<(), Failure> {
    use std::io::BufRead;
    let invalid = |message: String| Failure { message, reported: true, memory: false };
    let read_error = |e: std::io::Error| invalid(format!("Cannot read shader translation results: {}", e));
    let file = fs::File::open(stage.join("results.tsv")).map_err(read_error)?;
    let mut lines = std::io::BufReader::new(file).lines();
    let header = lines.next().ok_or_else(|| invalid("The shader translator wrote empty results.".into()))?.map_err(read_error)?;
    let status = header.split('\t').position(|s| s == "status").ok_or_else(|| invalid("Shader results have no status column.".into()))?;
    let error = header.split('\t').position(|s| s == "error").ok_or_else(|| invalid("Shader results have no error column.".into()))?;
    for line in lines {
        let line = line.map_err(read_error)?;
        if line.split('\t').nth(status) == Some("error") {
            let error = line.split('\t').nth(error).unwrap_or("");
            if error.starts_with("OutOfMemoryException:") || error.starts_with("System.OutOfMemoryException:") {
                return Err(Failure { message: "The shader translator reported an out-of-memory error for a shader in results.tsv.".into(), reported: true, memory: true });
            }
        }
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn run_native_with(
    dll: &Path, input: &Path, stage: &Path, cancel: &AtomicBool, gentle: bool, log: Option<&LogFn>,
    mut settings: resources::Settings, mut retry_settings: impl FnMut() -> Result<resources::Settings, String>, mut line: impl FnMut(&str),
) -> Result<bool, String> {
    use std::sync::atomic::Ordering;
    for attempt in 0..2 {
        if cancel.load(Ordering::SeqCst) {
            return Ok(false);
        }
        for name in [HARNESS_ERROR, HARNESS_PROGRESS] {
            match fs::remove_file(stage.join(name)) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(format!("cannot clear shader translator diagnostics: {}", e)),
            }
        }
        if let Some(log) = log { log(&settings.describe()); }
        let result = run_once(native_command(dll, input, stage, settings), stage, cancel, gentle, log, &mut line)
            .and_then(|completed| if completed { check_native_results(stage).map(|()| true) } else { Ok(false) });
        match result {
            Err(f) if attempt == 0 && settings.mode == Mode::Fast
                && (!f.reported || f.memory) => {
                if cancel.load(Ordering::SeqCst) { return Ok(false); }
                // Another application may have consumed RAM since the first attempt. Never enlarge the heap on retry.
                let fresh = retry_settings().map_err(|e| format!("{}\n{}", f.message, e))?;
                settings = resources::Settings { heap: settings.heap.min(fresh.heap), available: fresh.available, ..settings }.safer();
                if let Some(log) = log { log("Shader translation failed: retrying once with fewer workers and a smaller workstation-GC heap."); }
            }
            r => return r.map_err(|f| f.message),
        }
    }
    unreachable!("the second attempt always returns")
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::sync::atomic::Ordering;

    fn fixture(name: &str, script: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("harness_{name}_{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("spirv_harness"), format!("#!/bin/sh\nout=$4\nprintf '%s %s\\n' \"$DOTNET_gcServer\" \"$DOTNET_GCHeapHardLimit\" >> \"$out/calls\"\nprintf 'status\\terror\\n' > \"$out/results.tsv\"\n{script}\n")).unwrap();
        fs::set_permissions(dir.join("spirv_harness"), fs::Permissions::from_mode(0o755)).unwrap();
        fs::write(dir.join("kept.spv"), b"existing cache").unwrap();
        dir
    }

    fn fast() -> resources::Settings {
        resources::Settings { jobs: 6, mode: Mode::Fast, heap: 1536 << 20, available: Some(8 << 30) }
    }

    #[test]
    fn silent_failure_retries_once_without_erasing_cached_shaders() {
        let dir = fixture("silent", "if [ \"$DOTNET_gcServer\" = 1 ]; then exit 2; fi\nexit 0");
        // An old report must not classify this new, silent failure as a translator error.
        fs::write(dir.join(HARNESS_ERROR), "old error").unwrap();
        assert!(run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &AtomicBool::new(false), false, None, fast(), || Ok(fast()), |_| {}).unwrap());
        let calls = fs::read_to_string(dir.join("calls")).unwrap();
        assert_eq!(calls.lines().count(), 2);
        assert!(calls.starts_with("1 0x60000000\n0 "));
        assert_eq!(fs::read(dir.join("kept.spv")).unwrap(), b"existing cache");
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn reported_shader_error_is_not_retried() {
        let dir = fixture("reported", "printf 'Unsupported shader instruction' > \"$out/harness_error.txt\"\nexit 1");
        let error = run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &AtomicBool::new(false), false, None, fast(), || Ok(fast()), |_| {}).unwrap_err();
        assert!(error.contains("Unsupported shader instruction"));
        assert_eq!(fs::read_to_string(dir.join("calls")).unwrap().lines().count(), 1);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn reported_out_of_memory_gets_one_smaller_attempt() {
        let dir = fixture("oom", "if [ \"$DOTNET_gcServer\" = 1 ]; then printf 'System.OutOfMemoryException' > \"$out/harness_error.txt\"; exit 1; fi\nexit 0");
        assert!(run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &AtomicBool::new(false), false, None, fast(), || Ok(fast()), |_| {}).unwrap());
        let calls = fs::read_to_string(dir.join("calls")).unwrap();
        assert_eq!(calls.lines().count(), 2);
        assert!(calls.contains("0 0x20000000"));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn per_block_oom_with_exit_zero_is_retried() {
        let dir = fixture("block_oom", "if [ \"$DOTNET_gcServer\" = 1 ]; then printf 'error\\tOutOfMemoryException: Insufficient memory\\n' >> \"$out/results.tsv\"; fi\nexit 0");
        assert!(run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &AtomicBool::new(false), false, None, fast(), || Ok(fast()), |_| {}).unwrap());
        assert_eq!(fs::read_to_string(dir.join("calls")).unwrap().lines().count(), 2);
        assert_eq!(fs::read(dir.join("kept.spv")).unwrap(), b"existing cache");
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn persistent_per_block_oom_cannot_report_success() {
        let dir = fixture("persistent_oom", "printf 'error\\tOutOfMemoryException: Insufficient memory\\n' >> \"$out/results.tsv\"\nexit 0");
        let error = run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &AtomicBool::new(false), false, None, fast(), || Ok(fast()), |_| {}).unwrap_err();
        assert!(error.contains("out-of-memory"));
        assert_eq!(fs::read_to_string(dir.join("calls")).unwrap().lines().count(), 2);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn other_per_block_errors_do_not_trigger_a_memory_retry() {
        let dir = fixture("block_error", "printf 'error\\tNotSupportedException: instruction\\n' >> \"$out/results.tsv\"\nexit 0");
        assert!(run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &AtomicBool::new(false), false, None, fast(), || Ok(fast()), |_| {}).unwrap());
        assert_eq!(fs::read_to_string(dir.join("calls")).unwrap().lines().count(), 1);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn cancellation_does_not_retry_or_delete_the_cache() {
        let dir = fixture("cancel", "printf 'ready\\n'\nexec sleep 30");
        let cancel = AtomicBool::new(false);
        assert!(!run_native_with(&dir.join("spirv_harness.dll"), &dir, &dir, &cancel, false, None, fast(), || Ok(fast()), |_| {
            cancel.store(true, Ordering::SeqCst);
        }).unwrap());
        assert_eq!(fs::read_to_string(dir.join("calls")).unwrap().lines().count(), 1);
        assert_eq!(fs::read(dir.join("kept.spv")).unwrap(), b"existing cache");
        fs::remove_dir_all(dir).unwrap();
    }
}
