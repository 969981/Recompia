// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run shader setup: builds `shader_cache/spirv` next to pkp.exe from the user's own romfs and main.
//!
//! Two existing tools do the work, both shipped in the launcher's `tools/` folder:
//!   1. `tools/shaders/extract_corpus.py` (Python 3 with the `lz4` package) finds every GLSLC output in the romfs and in
//!      exefs/main and writes the distinct shader blocks to `shader_cache/corpus` (pack/<NN>.code, .ctrl, .glslc and
//!      unique.tsv with each block's pack refs: a few large files, issue #194);
//!   2. `tools/spirv_harness/spirv_harness.exe` (self-contained .NET publish) translates them to `<hash>.spv` plus the
//!      `results.tsv` rows the runtime reads.
//!
//! Resumable: the full block list is kept as `corpus/unique_all.tsv`; translation runs in chunks (the harness reads
//! `corpus/unique.tsv`, rewritten per chunk) into `shader_cache/stage`, and a chunk counts only once its `.spv` files are
//! moved into `spirv/` and its rows appended to `spirv/results.tsv`. A block with a row there is never translated again.
//! `shader_cache/setup.json` records what is done. The tools run at below-normal priority.
//! On Linux and macOS the tools run natively: the extractor with the host's Python, the translator as the host's own self-contained
//! publish (host.rs).
//! Compact storage (shader_archive.rs): when the cache has a shader archive, or a new setup asks for one (`Job::compact`), every chunk is
//! moved into the archive as soon as it is committed, so the loose files never pile up. A cache of loose files stays loose until the
//! player makes it smaller (main.rs compact_shaders).
//! Its only crate-local imports are host.rs (with download.rs) and shader_archive.rs, so `examples/shader_setup.rs` can build them alone.

use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::time::Duration;

/// Folder (in the game folder) the runtime opens by default; also what setup writes as `[nvn] shader_cache`.
pub const SPIRV_REL: &str = "shader_cache/spirv";
/// `[nvn] dump_missing`: the runtime writes the blocks the cache has no translation for here (the game builds some while
/// it runs, so the romfs corpus cannot have them). The launcher translates them after the game exits.
pub const MISSING_REL: &str = "shader_cache/missing";
/// Every run-time block seen so far, kept with its code (manifest.tsv, code_<hash>.bin, ctrl_<hash>.bin), so each Play
/// can check that all of them are translated without running the game.
pub const RUNTIME_REL: &str = "shader_cache/runtime";
/// Run-time blocks a translator fix changes whose old translation is in the shader archive (it cannot be deleted one by one there):
/// one hash per line. translate_missing translates them again, as if they had no translation, and empties the list.
const RETRANSLATE_REL: &str = "shader_cache/runtime/retranslate.txt";
/// Blocks per translator run. Every run pays its own process start (the .NET start and a cold JIT everywhere) and its own commit,
/// so a chunk of 1,000 cost about 7 s of overhead on a 24-thread Windows PC: 99 runs of 12 s for what one run of 413 s does. 7,000
/// blocks is about 1.8 GB of SPIR-V (preflight CHUNK_BYTES) and 15 runs, and a stopped run no longer loses its work (the harness
/// writes a results row per block as it goes), so a bigger chunk costs nothing when the player pauses.
const CHUNK: usize = 7000;
/// What a chunk of .spv files takes: the 2.0.1 corpus is 26.1 GB over 99,012 blocks, 1.8 GB per chunk (measured, every chunk of the
/// corpus). A RAM stage is used only with this much free, so a chunk bigger than that one still fits.
#[cfg(target_os = "linux")]
const RAM_STAGE_FREE: u64 = 3 << 30;

/// Where a chunk's loose .spv files are written before they are archived. A RAM filesystem when the platform has one with room: the
/// files are written once, read back within the same chunk and deleted, and on a Steam Deck's microSD card those writes and reads are
/// most of what the shader step costs. Elsewhere (and on Windows, which has no RAM file system) the game folder's cache.
pub fn stage_dir(game_dir: &Path) -> PathBuf {
    ram_stage().unwrap_or_else(|| disk_stage(game_dir))
}

/// The stage inside the game's cache, for a platform with no RAM filesystem (or one that is full or not writable).
fn disk_stage(game_dir: &Path) -> PathBuf {
    cache_dir(game_dir).join("stage")
}

/// True when `stage` is the RAM filesystem's: a chunk that does not fit there has to run from the game folder instead.
#[cfg(target_os = "linux")]
fn stage_is_ram(stage: &Path) -> bool {
    stage.starts_with("/dev/shm")
}

#[cfg(not(target_os = "linux"))]
fn stage_is_ram(_stage: &Path) -> bool {
    false
}

#[cfg(target_os = "linux")]
fn ram_stage() -> Option<PathBuf> {
    let shm = Path::new("/dev/shm");
    if !shm.is_dir() {
        return None;
    }
    // A Set up that was killed leaves its folder behind, and a chunk of SPIR-V in RAM is not something to leak on a 16 GB machine
    // (and a full tmpfs makes the next one fail). Every folder is named after the launcher process that owns it, so one whose
    // process is gone is stale.
    if let Ok(rd) = fs::read_dir(shm) {
        for e in rd.filter_map(Result::ok) {
            let name = e.file_name().to_string_lossy().to_string();
            let Some(pid) = name.strip_prefix("nxrt-stage-").and_then(|p| p.parse::<u32>().ok()) else { continue };
            if pid != std::process::id() && !Path::new(&format!("/proc/{}", pid)).exists() {
                // a folder, or anything else that took the name (remove_dir_all alone would leave a file behind for good)
                let p = e.path();
                if fs::remove_file(&p).is_err() {
                    let _ = fs::remove_dir_all(&p);
                }
            }
        }
    }
    if crate::host::free_bytes(shm)? < RAM_STAGE_FREE {
        return None;
    }
    // a mount that is there but not writable (a sandbox, a hardened distro) has to fall back here rather than fail a Set up halfway
    let probe = shm.join(format!("nxrt-stage-probe-{}", std::process::id()));
    if fs::write(&probe, b"x").is_err() {
        return None;
    }
    let _ = fs::remove_file(&probe);
    let dir = shm.join(format!("nxrt-stage-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    Some(dir)
}

#[cfg(not(target_os = "linux"))]
fn ram_stage() -> Option<PathBuf> {
    None
}

/// Workers for the extractor. It maps each asset file into memory and decompresses it, so one worker per CPU thread can
/// thrash a machine's memory and disk (issue #194: 15 workers on a hard drive); a few already saturate the disk. The
/// translator (spirv_harness) keeps the full `job.jobs`.
const EXTRACT_JOBS: usize = 4;
/// Translator revision. Raised when a translator fix changes shaders that are already translated: a finished cache of a lower
/// revision counts as unfinished, and Set up translates those shaders again (`stale_blocks`), the others are kept.
/// 1: an atomic whose result is read once ran twice (every such counter added its amount twice; spirv-call-args.patch).
/// 2: LOP3.LUT with a predicate destination read its sources one operand off and left the predicate false (#242).
pub const TRANSLATOR_REV: u32 = 2;
/// The blocks whose translation revision 2 changes: every block of the 2.0.1 corpus whose SPIR-V differs between the
/// translator before and after the #242 fix (tools/shaders/ampere/frontend/AmpereEmit.cs EmitLop3Ampere), one hash a line.
const REV2_BLOCKS: &str = include_str!("rev2_blocks.txt");
/// Threads for the stale-shader check (`stale_blocks`): it only reads small files, so the disk sets the pace, not the CPU.
const STALE_SCAN_THREADS: usize = 8;

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Marker {
    /// romfs the corpus was extracted from
    pub romfs: String,
    pub extracted: bool,
    pub blocks: usize,
    pub complete: bool,
    /// TRANSLATOR_REV of the finished cache (0: before revisions were recorded)
    pub translator: u32,
    /// "compact" (shader archive) or "loose": decided once, when this cache's first setup starts (Job::compact), so a setup that stops
    /// before its first chunk is packed keeps the choice. Empty: a cache from before the archive (it is loose). Packing and unpacking
    /// in Settings change it (set_storage).
    pub storage: String,
}

/// Where a setup job stands. The page gets the lowercase word: "idle", "running", "done", "error", "cancelled".
#[derive(Serialize, Clone, Copy, Default, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
#[allow(dead_code)] // the launcher builds every variant; the shader_setup example includes this file and builds only Running
pub enum RunState {
    #[default]
    Idle,
    Running,
    Done,
    Error,
    Cancelled,
}

impl std::fmt::Display for RunState {
    /// The same word the page gets (the setup log prints it).
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            RunState::Idle => "idle",
            RunState::Running => "running",
            RunState::Done => "done",
            RunState::Error => "error",
            RunState::Cancelled => "cancelled",
        })
    }
}

#[derive(Serialize, Clone, Default)]
pub struct Progress {
    pub state: RunState,
    /// "check", "extract", "translate", "finish"
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

pub struct Tools {
    pub python: PathBuf,
    pub extract: PathBuf,
    pub harness: PathBuf,
}

/// Where setup writes its log lines (the launcher's setup.log); a closure so this file needs no crate-local import.
pub type LogFn = std::sync::Arc<dyn Fn(&str) + Send + Sync>;

pub struct Job {
    pub game_dir: PathBuf,
    pub romfs: PathBuf,
    /// The base game's romfs layered under `romfs` (an update dumped without the base game's files under it); None: romfs alone.
    pub romfs_base: Option<PathBuf>,
    pub main: PathBuf,
    pub jobs: usize,
    /// "Gentle build": the tools run at idle priority (the default is below normal).
    pub idle: bool,
    pub log: Option<LogFn>,
    /// Test runs: translate at most this many blocks (0 = all); the setup is then never marked complete.
    pub limit: usize,
    /// A new cache goes into the shader archive (shader_archive.rs). Only decides for a cache with no translated shader yet: an existing
    /// cache keeps the storage it has (one with an archive stays compact, one of loose files stays loose).
    pub compact: bool,
}

impl Job {
    /// What the marker records as the extracted romfs: the romfs, and the base romfs under it when there is one.
    pub fn romfs_key(&self) -> String {
        let f = |p: &Path| p.to_string_lossy().replace('\\', "/");
        match &self.romfs_base {
            Some(b) => format!("{} over {}", f(&self.romfs), f(b)),
            None => f(&self.romfs),
        }
    }
}

pub fn cache_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("shader_cache")
}

pub fn read_marker(game_dir: &Path) -> Marker {
    fs::read_to_string(cache_dir(game_dir).join("setup.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn write_marker(game_dir: &Path, m: &Marker) -> Result<(), String> {
    // through a temporary file: a marker cut short by a crash would read back as no progress and start the hours-long setup over
    let p = cache_dir(game_dir).join("setup.json");
    let tmp = cache_dir(game_dir).join("setup.json.tmp");
    fs::write(&tmp, serde_json::to_string_pretty(m).unwrap_or_default()).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, &p).map_err(|e| format!("cannot replace {}: {}", p.display(), e))
}

/// How this cache is (or will be) stored, with `choice` the setup's Job::compact: a cache with an archive is compact, otherwise the
/// storage setup.json recorded, otherwise (nothing decided yet) the choice when no shader is translated yet, else loose. Setup and
/// the preflight's disk estimate both ask this.
pub fn storage_of(game_dir: &Path, marker: &Marker, choice: bool) -> &'static str {
    if crate::shader_archive::exists(game_dir) || marker.storage == "compact" {
        "compact"
    } else if marker.storage.is_empty() && choice && crate::shader_archive::loose_files(game_dir).is_empty() {
        "compact"
    } else {
        "loose"
    }
}

/// Records how the cache is stored ("compact" or "loose"), after Settings packed or unpacked it.
pub fn set_storage(game_dir: &Path, storage: &str) -> Result<(), String> {
    let mut m = read_marker(game_dir);
    m.storage = storage.to_string();
    write_marker(game_dir, &m)
}

/// True when a finished setup's cache is in place.
pub fn is_complete(game_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.complete && m.translator >= TRANSLATOR_REV && game_dir.join(SPIRV_REL).join("results.tsv").is_file()
}

/// Records a finished cache (the macOS setup translates it itself, outside shaders::run): `is_complete` then says so and
/// Set up does not run again until the RomFS or the translator changes.
pub fn mark_complete(game_dir: &Path, romfs: &str, blocks: usize) -> Result<(), String> {
    let m = Marker { romfs: romfs.to_string(), extracted: true, blocks, complete: true, translator: TRANSLATOR_REV, storage: "loose".into() };
    write_marker(game_dir, &m)
}

/// True when the cache was finished by an older translator that a later fix changes: Set up has to run once more.
pub fn needs_retranslation(game_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.complete && m.translator < TRANSLATOR_REV
}

/// The blocks of `rows` (results.tsv rows) whose translation by revision `rev` the later revisions change. Before 1: every
/// module with an atomic instruction (OpAtomicExchange..OpAtomicXor, opcodes 229-242). Before 2: the blocks of REV2_BLOCKS,
/// and every block the game built while it ran (not in `corpus`, so not in the list; a few hundred, among them the lighting
/// shader that draws the #242 outline).
/// The rev < 1 check reads every translated shader (~99,000 files): on a hard drive with an antivirus scanning each file that
/// took longer than players waited, with no progress shown (#269). So it runs on several threads, reports `progress(done, total)`
/// and stops on `cancel` (None).
fn stale_blocks(
    spirv: &Path,
    archive: &crate::shader_archive::Archive,
    rows: &[String],
    corpus: &HashSet<&str>,
    rev: u32,
    cancel: &AtomicBool,
    mut progress: impl FnMut(u64, u64),
) -> Option<HashSet<String>> {
    let mut out = HashSet::new();
    if rev >= TRANSLATOR_REV {
        return Some(out);
    }
    if rev < 2 {
        let list: HashSet<&str> = REV2_BLOCKS.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
        out.extend(rows.iter().map(|r| first_col(r)).filter(|h| list.contains(h) || !corpus.contains(h)).map(str::to_string));
    }
    if rev >= 1 {
        return Some(out);
    }
    let todo: Vec<&str> = rows.iter().map(|r| first_col(r)).filter(|h| !out.contains(*h)).collect();
    let total = todo.len() as u64;
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).clamp(1, STALE_SCAN_THREADS);
    let per = todo.len().div_ceil(threads).max(1);
    let (checked, running) = (AtomicU64::new(0), AtomicUsize::new(0));
    let found: Vec<String> = std::thread::scope(|s| {
        let parts: Vec<_> = todo
            .chunks(per)
            .map(|part| {
                running.fetch_add(1, Ordering::SeqCst);
                let (checked, running) = (&checked, &running);
                s.spawn(move || {
                    let mut hits = Vec::new();
                    for h in part {
                        if cancel.load(Ordering::SeqCst) {
                            break;
                        }
                        if crate::shader_archive::read_translation(spirv, archive, h).is_some_and(|b| has_atomic(&b)) {
                            hits.push(h.to_string());
                        }
                        checked.fetch_add(1, Ordering::Relaxed);
                    }
                    running.fetch_sub(1, Ordering::SeqCst);
                    hits
                })
            })
            .collect();
        while running.load(Ordering::SeqCst) > 0 {
            progress(checked.load(Ordering::Relaxed), total);
            std::thread::sleep(Duration::from_millis(250));
        }
        parts.into_iter().flat_map(|p| p.join().unwrap_or_default()).collect()
    });
    if cancel.load(Ordering::SeqCst) {
        return None;
    }
    progress(total, total);
    out.extend(found);
    Some(out)
}

/// True when the SPIR-V module has an atomic instruction (OpAtomicExchange..OpAtomicXor, opcodes 229-242).
fn has_atomic(b: &[u8]) -> bool {
    let w: Vec<u32> = b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
    let mut i = 5;
    while i < w.len() {
        let (wc, op) = ((w[i] >> 16) as usize, w[i] & 0xffff);
        if wc == 0 {
            return false;
        }
        if (229..=242).contains(&op) {
            return true;
        }
        i += wc;
    }
    false
}

pub(crate) fn quiet(cmd: &mut Command, idle: bool) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, or IDLE_PRIORITY_CLASS when idle (the tools' own child processes inherit it)
        cmd.creation_flags(0x0800_0000 | if idle { 0x0000_0040 } else { 0x0000_4000 });
    }
    #[cfg(not(windows))]
    crate::host::lower_priority(cmd, idle);
}

fn python_has_lz4(py: &Path) -> bool {
    let mut c = crate::host::command(py);
    c.args(["-I", "-B", "-c", "import lz4.block"]).stdout(Stdio::null()).stderr(Stdio::null());
    quiet(&mut c, false);
    c.status().map(|s| s.success()).unwrap_or(false)
}

fn on_path(exe: &str) -> Vec<PathBuf> {
    std::env::var_os("PATH").map(|p| std::env::split_paths(&p).map(|d| d.join(exe)).filter(|f| f.is_file()).collect()).unwrap_or_default()
}

/// Where the package keeps the translator: `tools/spirv_harness` in the Windows zip, `tools/harness` in the macOS and Linux
/// packages (the native apphost, which they make executable).
pub fn harness_dir(tools_dir: &Path) -> PathBuf {
    tools_dir.join(if cfg!(windows) { "spirv_harness" } else { "harness" })
}

/// The translator in `tools_dir`: `spirv_harness.exe` on Windows, the native `spirv_harness` apphost elsewhere.
pub fn harness_exe(tools_dir: &Path) -> PathBuf {
    harness_dir(tools_dir).join(if cfg!(windows) { "spirv_harness.exe" } else { "spirv_harness" })
}

/// The files of the translator's self-contained publish without which it cannot start at all (one empty file is a cut-off copy
/// or extraction, #366).
fn harness_files() -> &'static [&'static str] {
    if cfg!(windows) {
        &["spirv_harness.exe", "spirv_harness.dll", "spirv_harness.deps.json", "spirv_harness.runtimeconfig.json", "hostfxr.dll", "hostpolicy.dll", "coreclr.dll"]
    } else if cfg!(target_os = "macos") {
        &["spirv_harness", "spirv_harness.dll", "spirv_harness.deps.json", "spirv_harness.runtimeconfig.json", "libhostfxr.dylib", "libhostpolicy.dylib", "libcoreclr.dylib"]
    } else {
        &["spirv_harness", "spirv_harness.dll", "spirv_harness.deps.json", "spirv_harness.runtimeconfig.json", "libhostfxr.so", "libhostpolicy.so", "libcoreclr.so"]
    }
}

/// Finds the tools in `tools_dir` (the launcher's `tools/` folder). Python: `tools/python/python.exe` (Linux:
/// `tools/linux/python/bin/python3`), else the first python.exe (python3) on PATH that has `lz4`. Every missing piece is named
/// in the error.
pub fn find_tools(tools_dir: &Path) -> Result<Tools, String> {
    let mut missing = Vec::new();
    let harness_dir = harness_dir(tools_dir);
    let harness = harness_exe(tools_dir);
    let broken: Vec<&str> = harness_files().iter().copied().filter(|f| fs::metadata(harness_dir.join(f)).map_or(true, |m| m.len() == 0)).collect();
    if !broken.is_empty() {
        missing.push(format!("the shader translator ({}: {} missing or empty)", harness_dir.display(), broken.join(", ")));
    }
    let extract = tools_dir.join("shaders").join("extract_corpus.py");
    if !extract.is_file() {
        missing.push(format!("the shader extractor ({})", extract.display()));
    }
    let bundled = crate::host::python(tools_dir);
    let mut candidates = vec![bundled.clone()];
    candidates.extend(on_path(if cfg!(windows) { "python.exe" } else { "python3" }));
    let python = candidates.into_iter().filter(|p| p.is_file()).find(|p| python_has_lz4(p));
    if python.is_none() {
        missing.push(format!(
            "Python 3 with the lz4 package ({}, or python.exe on PATH with `pip install lz4`)",
            bundled.display()
        ));
    }
    if !missing.is_empty() {
        return Err(format!("Shader setup cannot run, missing: {}. Re-extract the launcher's release folder so tools/ is complete.", missing.join("; ")));
    }
    Ok(Tools { python: python.unwrap(), extract, harness })
}

/// A progress line of a tool ("scan: 12 of 99", "  500/1000  3.2 s"): counted by the progress bar, not worth a log line.
pub(crate) fn is_progress_line(l: &str) -> bool {
    l.starts_with("scan: ") || l.trim().split('/').next().map_or(false, |x| !x.is_empty() && x.bytes().all(|b| b.is_ascii_digit()) && l.contains('/'))
}

/// Runs a tool, handing each stdout line to `line`; kills it when `cancel` is set. Ok(true) = exit 0, Ok(false) = cancelled.
/// `log` gets the command line, the tool's stderr and non-progress stdout, and on failure the end of its output.
/// `limit`: killed after that long, an error.
pub(crate) fn run_tool(mut cmd: Command, cancel: &AtomicBool, idle: bool, log: Option<&LogFn>, limit: Option<Duration>, mut line: impl FnMut(&str)) -> Result<bool, String> {
    quiet(&mut cmd, idle);
    let name = cmd.get_program().to_string_lossy().to_string();
    let say = |t: &str| {
        if let Some(f) = log {
            f(t)
        }
    };
    say(&format!("run: {} {}", name, cmd.get_args().map(|a| a.to_string_lossy().to_string()).collect::<Vec<_>>().join(" ")));
    let mut child: Child = crate::proc_tree::spawn(cmd.stdout(Stdio::piped()).stderr(Stdio::piped())).map_err(|e| format!("could not start {}: {}", name, e))?;
    let (tx, rx) = mpsc::channel::<String>();
    let out = child.stdout.take().unwrap();
    let tx2 = tx.clone();
    std::thread::spawn(move || crate::child_lines::lines(out).for_each(|l| drop(tx2.send(l))));
    let err = child.stderr.take().unwrap();
    let (etx, erx) = mpsc::channel::<String>();
    let elog = log.cloned();
    std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in crate::child_lines::lines(err) {
            if let Some(f) = &elog {
                f(&format!("stderr: {}", l));
            }
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        let _ = etx.send(tail.join("\n"));
    });
    drop(tx);
    let deadline = limit.map(|d| std::time::Instant::now() + d);
    let mut out_tail: Vec<String> = Vec::new();
    loop {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => {
                if !is_progress_line(&l) {
                    say(&format!("out: {}", l));
                }
                out_tail.push(l.clone());
                if out_tail.len() > 12 {
                    out_tail.remove(0);
                }
                line(&l)
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                // stdout closed: the process is ending. Polled, so Pause and the time limit still apply while it does
                if let Some(st) = child.try_wait().map_err(|e| e.to_string())? {
                    if cancel.load(Ordering::SeqCst) {
                        say(&format!("stopped: {} (paused by the player)", name));
                        return Ok(false);
                    }
                    if st.success() {
                        say(&format!("done: {}", name));
                        return Ok(true);
                    }
                    let tail = erx.recv_timeout(Duration::from_secs(5)).unwrap_or_default();
                    say(&format!("FAILED: {} ({}). Last output:\n{}\nLast error output:\n{}", name, st, out_tail.join("\n"), tail));
                    return Err(format!("{} failed ({}). {}", name, st, tail.trim()));
                }
                std::thread::sleep(Duration::from_millis(200));
            }
        }
        if cancel.load(Ordering::SeqCst) {
            crate::host::kill(&mut child);
            let _ = child.wait();
            say(&format!("stopped: {} (paused by the player)", name));
            return Ok(false);
        }
        if deadline.is_some_and(|d| std::time::Instant::now() >= d) {
            crate::host::kill(&mut child);
            let _ = child.wait();
            say(&format!("stopped: {} (still running after {} s)", name, limit.unwrap_or_default().as_secs()));
            return Err(format!("{} did not end in time", name));
        }
    }
}

/// Header + rows of a tsv file (rows without the trailing newline).
fn read_tsv(p: &Path) -> Result<(String, Vec<String>), String> {
    let text = fs::read_to_string(p).map_err(|e| format!("cannot read {}: {}", p.display(), e))?;
    let mut it = text.lines();
    let header = it.next().unwrap_or("").to_string();
    Ok((header, it.filter(|l| !l.is_empty()).map(|l| l.to_string()).collect()))
}

fn first_col(row: &str) -> &str {
    row.split('\t').next().unwrap_or("")
}

/// The rows of a harness run, with a killed run's half-written last row dropped (a row without every column is not a translation).
fn stage_rows(stage: &Path) -> Result<(String, Vec<String>), String> {
    let (rheader, mut rrows) = read_tsv(&stage.join("results.tsv"))?;
    let cols = rheader.split('\t').count();
    rrows.retain(|r| r.split('\t').count() == cols);
    Ok((rheader, rrows))
}

/// Appends a run's rows to spirv/results.tsv. They go last: a row is what makes a block count as done, so it is written only once the
/// block's SPIR-V is in the cache (moved there or archived).
fn append_rows(spirv: &Path, rheader: &str, rrows: &[String]) -> Result<usize, String> {
    let results = spirv.join("results.tsv");
    let new_file = !results.is_file();
    let mut out = fs::OpenOptions::new().create(true).append(true).open(&results).map_err(|e| format!("cannot write {}: {}", results.display(), e))?;
    let mut text = String::new();
    if new_file {
        text.push_str(rheader);
        text.push('\n');
    }
    for r in rrows {
        text.push_str(r);
        text.push('\n');
    }
    out.write_all(text.as_bytes()).map_err(|e| format!("cannot write {}: {}", results.display(), e))?;
    Ok(rrows.len())
}

/// Moves one file, copying it when the two are on different file systems (a RAM stage and the game folder are).
fn move_file(src: &Path, dst: &Path) -> Result<(), String> {
    let _ = fs::remove_file(dst);
    if fs::rename(src, dst).is_ok() {
        return Ok(());
    }
    fs::copy(src, dst).map_err(|e| format!("cannot move {}: {}", src.display(), e))?;
    let _ = fs::remove_file(src);
    Ok(())
}

/// Moves a harness run's .spv files from `stage` into `spirv` (the loose storage), then appends its rows to spirv/results.tsv.
/// Returns the number of rows.
fn commit_stage(stage: &Path, spirv: &Path) -> Result<usize, String> {
    let (rheader, rrows) = stage_rows(stage)?;
    for r in &rrows {
        let h = first_col(r);
        let src = stage.join(format!("{}.spv", h));
        if src.is_file() {
            move_file(&src, &spirv.join(format!("{}.spv", h)))?;
        }
    }
    append_rows(spirv, &rheader, &rrows)
}

fn col(row: &str, k: usize) -> &str {
    row.split('\t').nth(k).unwrap_or("")
}

/// True when no run-time block was ever collected for this cache: the warm-up run has not happened yet.
pub fn needs_warm_up(game_dir: &Path) -> bool {
    !game_dir.join(RUNTIME_REL).join("manifest.tsv").is_file()
}

/// Before every start: moves what the game dumped to MISSING_REL (by any run, also one started without the launcher)
/// into RUNTIME_REL, then translates every block listed there that has no .spv in the cache. Ok(n) = blocks translated.
pub fn translate_missing(game_dir: &Path, tools_dir: &Path) -> Result<usize, String> {
    let missing = game_dir.join(MISSING_REL);
    let runtime = game_dir.join(RUNTIME_REL);
    let list = runtime.join("manifest.tsv");
    // manifest columns (runtime nvn_shaders.cpp dump_missing_block): 10 = hash, 11 = code file, 12 = control file
    if let Ok((header, rows)) = read_tsv(&missing.join("manifest.tsv")) {
        fs::create_dir_all(&runtime).map_err(|e| format!("cannot create {}: {}", runtime.display(), e))?;
        let mut known: HashSet<String> = read_tsv(&list).map(|(_, r)| r.iter().map(|x| col(x, 10).to_string()).collect()).unwrap_or_default();
        let mut text = if fs::metadata(&list).map(|m| m.len() == 0).unwrap_or(true) { format!("{}\n", header) } else { String::new() };
        for r in &rows {
            // the same block twice (several runs appended to one manifest before it was merged): the first one counts
            if !known.insert(col(r, 10).to_string()) {
                continue;
            }
            for f in [col(r, 11), col(r, 12)] {
                fs::rename(missing.join(f), runtime.join(f)).map_err(|e| format!("cannot move {}: {}", f, e))?;
            }
            text.push_str(r);
            text.push('\n');
        }
        let mut out = fs::OpenOptions::new().create(true).append(true).open(&list).map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
        out.write_all(text.as_bytes()).map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
        let _ = fs::remove_dir_all(&missing);
    }

    let spirv = game_dir.join(SPIRV_REL);
    let Ok((header, rows)) = read_tsv(&list) else { return Ok(0) };
    let archive = crate::shader_archive::Archive::open(game_dir);
    let again: HashSet<String> = fs::read_to_string(game_dir.join(RETRANSLATE_REL)).map(|t| t.lines().map(|l| l.trim().to_string()).collect()).unwrap_or_default();
    // ponytail: a block the translator fails on has no .spv and is tried again at every start (a second or two)
    let todo: Vec<&String> = rows
        .iter()
        .filter(|r| again.contains(col(r, 10)) || !crate::shader_archive::has_translation(&spirv, &archive, col(r, 10)))
        .collect();
    if todo.is_empty() {
        let _ = fs::remove_file(game_dir.join(RETRANSLATE_REL));
        return Ok(0);
    }
    // the harness reads the code and control files relative to its manifest: absolute paths (as the translator sees them)
    // point it at RUNTIME_REL
    let input = cache_dir(game_dir).join("runtime_todo");
    let stage = cache_dir(game_dir).join("stage_missing");
    let _ = fs::remove_dir_all(&input);
    let _ = fs::remove_dir_all(&stage);
    fs::create_dir_all(&input).map_err(|e| format!("cannot create {}: {}", input.display(), e))?;
    let mut text = format!("{}\n", header);
    for r in &todo {
        let mut f: Vec<String> = r.split('\t').map(String::from).collect();
        for k in [11, 12] {
            if let Some(v) = f.get_mut(k) {
                *v = runtime.join(&*v).to_string_lossy().replace('\\', "/");
            }
        }
        text.push_str(&f.join("\t"));
        text.push('\n');
    }
    fs::write(input.join("manifest.tsv"), text).map_err(|e| format!("cannot write {}: {}", input.display(), e))?;
    let jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    let exe = harness_exe(tools_dir);
    let mut mode = crate::harness::Mode::Fast;
    if !crate::harness::run(&exe, &input, &stage, jobs, &mut mode, &AtomicBool::new(false), false, None, |_| {})? {
        return Err("the shader translator stopped".into());
    }
    let n = commit_stage(&stage, &spirv)?;
    let _ = fs::remove_dir_all(&stage);
    let _ = fs::remove_dir_all(&input);
    let _ = fs::remove_file(game_dir.join(RETRANSLATE_REL));
    // a compact cache stays compact: the new translations join the archive (a small segment)
    if crate::shader_archive::exists(game_dir) {
        let hashes: Vec<String> = todo.iter().map(|r| col(r, 10).to_string()).collect();
        crate::shader_archive::compact(game_dir, None, Some(&hashes), &AtomicBool::new(false), |_, _| {})?;
    }
    Ok(n)
}

/// Seconds the warm-up run plays. The opening asks for its run-time blocks about 3 to 25 s after start.
pub const WARMUP_SECONDS: u32 = 60;

/// Plays the game once without a window or sound, for WARMUP_SECONDS, so the blocks it builds while it runs (the romfs
/// corpus cannot have them) are collected and translated before a real start. A fresh save folder in the cache, so the
/// player's own save is never touched. Ok(None) = cancelled, else how many blocks were translated.
pub fn warm_up(game_dir: &Path, tools_dir: &Path, cancel: &AtomicBool) -> Result<Option<usize>, String> {
    let scratch = cache_dir(game_dir).join("warmup");
    let _ = fs::remove_dir_all(&scratch);
    fs::create_dir_all(scratch.join("save")).map_err(|e| format!("cannot create {}: {}", scratch.display(), e))?;
    if let Some(install) = tools_dir.parent() {
        crate::host::sync_runtime_files(install, game_dir)?;
    }
    let mut cmd = crate::host::command(&crate::host::game_exe(game_dir));
    cmd.current_dir(game_dir).arg("--headless").args(["--max-seconds", &WARMUP_SECONDS.to_string()]).arg("--log").arg(scratch.join("warmup.log"));
    // environment variables win over pkp.ini
    cmd.env("NXRT_FS_SAVE_DIR", scratch.join("save")).env("NXRT_NVN_DUMP_MISSING", MISSING_REL).env("NXRT_AUDIO_SINK", "null");
    // it ends with exit code 124 when the time is up: any end is fine, a crash only means fewer blocks were collected. One
    // still running 4 minutes later (a first start also translates the first blocks) is stopped.
    if let Ok(false) = run_tool(cmd, cancel, false, None, Some(Duration::from_secs(WARMUP_SECONDS as u64 + 240)), |_| {}) {
        return Ok(None);
    }
    let n = translate_missing(game_dir, tools_dir)?;
    // A run that collected nothing is retried at the next Play/Set up, but only twice, so a game that never dumps a run-time
    // block does not warm up on every start. The retries are counted in a small file next to the manifest.
    let list = game_dir.join(RUNTIME_REL).join("manifest.tsv");
    if list.is_file() {
        let _ = fs::remove_file(game_dir.join(RUNTIME_REL).join("warmup_empty"));
    } else {
        let counter = game_dir.join(RUNTIME_REL).join("warmup_empty");
        let tried = fs::read_to_string(&counter).ok().and_then(|s| s.trim().parse::<u32>().ok()).unwrap_or(0) + 1;
        let _ = fs::create_dir_all(game_dir.join(RUNTIME_REL));
        if tried >= 3 {
            fs::write(&list, "").map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
            let _ = fs::remove_file(&counter);
        } else {
            fs::write(&counter, tried.to_string()).map_err(|e| format!("cannot write {}: {}", counter.display(), e))?;
        }
    }
    let _ = fs::remove_dir_all(&scratch);
    Ok(Some(n))
}

/// What every start and every setup does: the warm-up run once, afterwards only the check of the collected blocks.
pub fn prepare(game_dir: &Path, tools_dir: &Path, cancel: &AtomicBool) -> Result<Option<usize>, String> {
    if needs_warm_up(game_dir) {
        warm_up(game_dir, tools_dir, cancel)
    } else {
        translate_missing(game_dir, tools_dir).map(Some)
    }
}

/// One chunk's archive work, handed to the thread that packs beside the translator (`run`).
struct PackJob {
    game_dir: PathBuf,
    /// the stage this chunk was translated into (a RAM filesystem on Linux, the game folder elsewhere)
    stage: PathBuf,
    spirv: PathBuf,
    /// the hashes this chunk translated, so the packer takes its files and not the next chunk's
    hashes: Vec<String>,
    header: String,
    rows: Vec<String>,
    threads: usize,
}

/// How many cores the archive step may use while it runs beside the translator: what the translator leaves, but at least two. It is
/// mostly waiting on the card, so two threads keep up with a translation that lasts minutes (a chunk's segments are a fraction of it).
fn pack_threads(jobs: usize) -> usize {
    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    cores.saturating_sub(jobs).clamp(2, 16)
}

/// The whole setup. `report` gets every progress change. Ok(true) = finished (or the limit reached), Ok(false) = cancelled.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Progress)) -> Result<bool, String> {
    let cache = cache_dir(&job.game_dir);
    let corpus = cache.join("corpus");
    let mut stage = stage_dir(&job.game_dir);
    // a stage folder left by a Set up that was killed: its rows would look like this run's (a one-shot run cleared it itself)
    let _ = fs::remove_dir_all(&stage);
    let spirv = job.game_dir.join(SPIRV_REL);
    fs::create_dir_all(&spirv).map_err(|e| format!("cannot create {}: {}", spirv.display(), e))?;
    crate::shader_archive::clean(&job.game_dir);
    let romfs_s = job.romfs_key();
    let mut marker = read_marker(&job.game_dir);
    if marker.romfs != romfs_s {
        marker = Marker { romfs: romfs_s.clone(), ..Default::default() };
    }
    // the storage this cache keeps (Job::compact): decided once, when nothing is translated yet; a cache with an archive is compact
    marker.storage = storage_of(&job.game_dir, &marker, job.compact).into();
    let compact = marker.storage == "compact";
    if let Some(l) = &job.log {
        l(&format!("shader storage: {}", if compact { "compact (shader archive)" } else { "loose .spv files" }));
    }
    marker.complete = false;
    write_marker(&job.game_dir, &marker)?;
    let extract_jobs = job.jobs.clamp(1, EXTRACT_JOBS).to_string();
    let p = |phase: &str, done: u64, total: u64, message: String| Progress { state: RunState::Running, phase: phase.into(), done, total, message };

    // 1. extract (skipped when this romfs was already extracted)
    let all = corpus.join("unique_all.tsv");
    if !(marker.extracted && all.is_file()) {
        report(p("extract", 0, 0, "Finding the game's shaders in your game files...".into()));
        let _ = fs::remove_file(corpus.join("unique.tsv"));
        let mut cmd = crate::host::command(&tools.python);
        cmd.arg("-I").arg("-B").arg(&tools.extract).arg("--romfs").arg(&job.romfs).arg("--main").arg(&job.main).arg("--out").arg(&corpus);
        if let Some(b) = &job.romfs_base {
            cmd.arg("--romfs-base").arg(b);
        }
        cmd.args(["--jobs", &extract_jobs, "--progress"]);
        let ok = run_tool(cmd, cancel, job.idle, job.log.as_ref(), None, |l| {
            if let Some(rest) = l.strip_prefix("scan: ") {
                let mut it = rest.split_whitespace().filter_map(|x| x.parse::<u64>().ok());
                if let (Some(d), Some(t)) = (it.next(), it.next()) {
                    report(p("extract", d, t, format!("Scanning game files: {} of {}", d, t)));
                }
            }
        })?;
        if !ok {
            return Ok(false);
        }
        fs::rename(corpus.join("unique.tsv"), &all).map_err(|e| format!("the extractor wrote no block list: {}", e))?;
        marker.extracted = true;
        write_marker(&job.game_dir, &marker)?;
    }

    // 2. translate what spirv/results.tsv does not list yet
    let (uheader, urows) = read_tsv(&all)?;
    marker.blocks = urows.len();
    write_marker(&job.game_dir, &marker)?;
    let results = spirv.join("results.tsv");
    let (_, rrows) = if results.is_file() { read_tsv(&results)? } else { (String::new(), Vec::new()) };
    let mut done: HashSet<String> = rrows.iter().map(|r| first_col(r).to_string()).collect();
    // a cache of an older translator: the blocks a later fix changes are translated again. Those of the corpus here; those the
    // game built while it ran (not in the corpus) lose their .spv, so translate_missing (prepare, after setup) does them.
    let archive = crate::shader_archive::Archive::open(&job.game_dir);
    let corpus_rows: HashSet<&str> = urows.iter().map(|r| first_col(r)).collect();
    if marker.translator < TRANSLATOR_REV && !rrows.is_empty() {
        report(p("recheck", 0, 0, "Checking the shaders an earlier version translated...".into()));
        if let Some(l) = &job.log {
            l(&format!("checking {} translated shaders for translator revision {} -> {}", rrows.len(), marker.translator, TRANSLATOR_REV));
        }
    }
    // setup.log gets a line per 5% from the reports (main.rs), so a slow disk shows up there too
    let Some(stale) = stale_blocks(&spirv, &archive, &rrows, &corpus_rows, marker.translator, cancel, |d, t| {
        report(p("recheck", d, t, format!("Checking the shaders an earlier version translated: {} of {}", d, t)))
    }) else {
        return Ok(false);
    };
    if !stale.is_empty() {
        let mut again = String::new();
        for h in &stale {
            done.remove(h);
            if !corpus_rows.contains(h.as_str()) {
                let _ = fs::remove_file(spirv.join(format!("{}.spv", h)));
                if archive.contains(h) {
                    again.push_str(h);
                    again.push('\n');
                }
            }
        }
        if !again.is_empty() {
            let p = job.game_dir.join(RETRANSLATE_REL);
            let _ = fs::create_dir_all(job.game_dir.join(RUNTIME_REL));
            let mut f = fs::OpenOptions::new().create(true).append(true).open(&p).map_err(|e| format!("cannot write {}: {}", p.display(), e))?;
            f.write_all(again.as_bytes()).map_err(|e| format!("cannot write {}: {}", p.display(), e))?;
        }
        if let Some(l) = &job.log {
            l(&format!("translator revision {} -> {}: {} shaders translated again", marker.translator, TRANSLATOR_REV, stale.len()));
        }
    }
    let mut todo: Vec<&String> = urows.iter().filter(|r| !done.contains(first_col(r))).collect();
    let limited = job.limit > 0 && todo.len() > job.limit;
    if job.limit > 0 {
        todo.truncate(job.limit);
    }
    let total = urows.len() as u64;
    let mut finished = (urows.len() - urows.iter().filter(|r| !done.contains(first_col(r))).count()) as u64;
    report(p("translate", finished, total, format!("Translating shaders: {} of {}", finished, total)));
    // One translator process for the whole translation (`--serve`): a chunk no longer pays the .NET start, the cold JIT and the
    // thread pool ramp, which was about 3 s of every chunk. A process that dies is started again, in Safe mode when it died without
    // a report (the same rule a one-shot run gets in harness::run), and its chunk runs from its start: nothing of a dead chunk was
    // committed.
    let mut worker: Option<crate::harness::Worker> = None;
    let mut mode = crate::harness::Mode::Fast;
    // The archive step runs beside the translator instead of after it: a chunk is packed while the next one is translated, so the
    // segments' 6.6 GB are written during the minutes of translation rather than after them. On a Steam Deck that write is most of
    // what the shader step spent outside the translator, and it is card time, not CPU: the packer asks for the cores the translator
    // leaves (two on a Deck) and the translator never waits for it. One job is in flight at a time, so the stage never holds more
    // than two chunks.
    let pack_cores = pack_threads(job.jobs);
    let (pack_tx, pack_rx) = mpsc::sync_channel::<PackJob>(1);
    let packer = compact.then(|| {
        std::thread::spawn(move || -> Result<(), String> {
            for j in pack_rx.iter() {
                crate::shader_archive::compact_with(&j.game_dir, Some(&j.stage), Some(&j.hashes), &AtomicBool::new(false), j.threads, |_, _| {})?;
                // The rows go last, and only once their SPIR-V is in a segment: a row is what makes a block count as done, so it is
                // written only once the block's SPIR-V is in the cache. A segment that cannot be written fails the whole setup.
                append_rows(&j.spirv, &j.header, &j.rows)?;
            }
            Ok(())
        })
    });
    let ran = (|| -> Result<bool, String> {
        for chunk in todo.chunks(CHUNK) {
            let mut f = fs::File::create(corpus.join("unique.tsv")).map_err(|e| e.to_string())?;
            writeln!(f, "{}", uheader).map_err(|e| e.to_string())?;
            for r in chunk {
                writeln!(f, "{}", r).map_err(|e| e.to_string())?;
            }
            drop(f);
            let base = finished;
            let ok = loop {
                if worker.is_none() {
                    worker = Some(crate::harness::Worker::start(&tools.harness, &stage, job.jobs, mode, job.idle)?);
                }
                let res = worker.as_mut().unwrap().run_chunk(&corpus, cancel, job.log.as_ref(), |l| {
                    // "  <n>/<count>  <s> s" every 500 blocks
                    if let Some(n) = l.trim().split('/').next().and_then(|x| x.parse::<u64>().ok()) {
                        report(p("translate", base + n, total, format!("Translating shaders: {} of {}", base + n, total)));
                    }
                });
                match res {
                    Ok(ok) => break ok,
                    Err(f) => {
                        worker = None;
                        // A RAM stage can run out of room (a chunk bigger than the free space, or /dev/shm shared with something else):
                        // the chunk runs again from the game folder rather than ending Set up. Nothing of it was committed.
                        if stage_is_ram(&stage) {
                            if let Some(l) = &job.log {
                                l("the RAM stage could not hold this chunk: the rest of this Set up uses the game folder");
                            }
                            let _ = fs::remove_dir_all(&stage);
                            stage = disk_stage(&job.game_dir);
                            continue;
                        }
                        if mode == crate::harness::Mode::Fast && !f.reported {
                            mode = crate::harness::Mode::Safe;
                            if let Some(l) = &job.log {
                                l(&format!(
                                    "The shader translator stopped without a report: running it again with workstation GC and {} jobs, and keeping that.",
                                    job.jobs.clamp(1, 2)
                                ));
                            }
                            continue;
                        }
                        return Err(f.message);
                    }
                }
            };
            // A run that was stopped still leaves every block it finished (the harness writes a row per block as it goes), so what it
            // got through is committed before setup reports the pause: resuming starts where it stopped. A run that never started has
            // no table to commit.
            let started = stage.join("results.tsv").is_file();
            if !ok && !started {
                return Ok(false);
            }
            let kept = if compact {
                // Straight from the stage into the archive: a fresh translation is never written loose to the game folder, so a chunk
                // costs one compressed write instead of a loose write, a read back and a delete. The packer does it while the next chunk
                // is translated, and appends this chunk's rows once their SPIR-V is in a segment.
                let (rheader, rrows) = stage_rows(&stage)?;
                let hashes: Vec<String> = rrows.iter().map(|r| first_col(r).to_string()).collect();
                let n = rrows.len();
                pack_tx
                    .send(PackJob {
                        game_dir: job.game_dir.clone(),
                        stage: stage.clone(),
                        spirv: spirv.clone(),
                        hashes,
                        header: rheader,
                        rows: rrows,
                        threads: pack_cores,
                    })
                    .map_err(|_| "the archive step stopped".to_string())?;
                n
            } else {
                let n = commit_stage(&stage, &spirv)?;
                let _ = fs::remove_dir_all(&stage);
                n
            };
            if !ok {
                if let Some(l) = &job.log {
                    l(&format!("paused: {} shaders translated before the pause are kept", kept));
                }
                return Ok(false);
            }
            finished += chunk.len() as u64;
            report(p("translate", finished, total, format!("Translating shaders: {} of {}", finished, total)));
        }
        Ok(true)
    })();
    if let Some(mut w) = worker.take() {
        w.stop();
    }
    // the last chunk's archive work finishes here: Set up reports nothing until every shader it counted is in a segment
    drop(pack_tx);
    let packed = match packer {
        Some(h) => h.join().unwrap_or_else(|_| Err("the archive step stopped without a reason".into())),
        None => Ok(()),
    };
    // the archive step's own failure is why a chunk could not be handed to it, so its reason is the one reported
    packed?;
    let done = ran?;
    if !done {
        return Ok(false);
    }
    let _ = fs::remove_dir_all(&stage);
    let _ = fs::remove_file(corpus.join("unique.tsv"));
    if limited {
        return Ok(true);
    }

    // 3. done: the corpus is only needed to translate, drop it (several GB)
    report(p("finish", total, total, "Cleaning up...".into()));
    let _ = fs::remove_dir_all(&corpus);
    marker.extracted = false;
    marker.complete = true;
    marker.translator = TRANSLATOR_REV;
    write_marker(&job.game_dir, &marker)?;
    Ok(true)
}
