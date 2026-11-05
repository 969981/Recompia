// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run shader setup: builds `shader_cache/spirv` next to pkp.exe from the user's own romfs and main.
//!
//! Two existing tools do the work, both shipped in the launcher's `tools/` folder:
//!   1. `tools/shaders/extract_corpus.py` (Python 3 with the `lz4` package) finds every GLSLC output in the romfs and in
//!      exefs/main and writes the distinct shader blocks to `shader_cache/corpus` (code/, ctrl/, glslc/, unique.tsv);
//!   2. `tools/spirv_harness/spirv_harness.exe` (self-contained .NET publish) translates them to `<hash>.spv` plus the
//!      `results.tsv` rows the runtime reads.
//!
//! Resumable: the full block list is kept as `corpus/unique_all.tsv`; translation runs in chunks (the harness reads
//! `corpus/unique.tsv`, rewritten per chunk) into `shader_cache/stage`, and a chunk counts only once its `.spv` files are
//! moved into `spirv/` and its rows appended to `spirv/results.tsv`. A block with a row there is never translated again
//! by the same translator: every row records the translator version that made it (column `translator`, see the harness's
//! TranslatorVersion.cs), and a row of any other version is dropped with its .spv before setup or the run-time check
//! translates anything (`drop_stale`), so a launcher with a newer translator re-translates an existing cache.
//! `shader_cache/setup.json` records what is done. The tools run at below-normal priority.
//!
//! Used shaders first: Pokopia's corpus has ~99k blocks (1-2 h, ~26 GB), but play runs use ~6k of them. The shipped list
//! `tools/shaders/used_shaders.txt` (hashes only, made by tools/shaders/scripts/used_shaders.py) plus this install's own
//! `shader_order.txt` (the runtime's record of every shader it used) are translated first (`Part::Used`); the marker
//! then says `ready` and Play turns on. The rest (`Part::Rest`) is translated in the background while the launcher is
//! open: idle priority, half the cores, in chunks of BG_CHUNK, and never while the game (or the warm-up, or Set up)
//! runs (`Background`). A running game reads results.tsv once at start and only sees new rows at its next start, so
//! translating during play would cost frames (CPU time and, on a handheld APU, power budget) for nothing, and pausing
//! means the cache is never written while the game reads it. Killing the harness loses at most one chunk; the next
//! launch continues. A block the game asks for that is not translated yet goes through `[nvn] dump_missing` as before
//! (drawn without it in that run, translated by `translate_missing` when the game exits).
//! No crate-local imports, so `examples/shader_setup.rs` can build this file alone.

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

/// Folder (in the game folder) the runtime opens by default; also what setup writes as `[nvn] shader_cache`.
pub const SPIRV_REL: &str = "shader_cache/spirv";
/// `[nvn] dump_missing`: the runtime writes the blocks the cache has no translation for here (the game builds some while
/// it runs, so the romfs corpus cannot have them). The launcher translates them after the game exits.
pub const MISSING_REL: &str = "shader_cache/missing";
/// Every run-time block seen so far, kept with its code (manifest.tsv, code_<hash>.bin, ctrl_<hash>.bin), so each Play
/// can check that all of them are translated without running the game.
pub const RUNTIME_REL: &str = "shader_cache/runtime";
const CHUNK: usize = 1000;
/// Chunk of the background part: a stop (the game starts) throws away at most this many blocks' work.
const BG_CHUNK: usize = 250;
/// In the tools folder: the shipped list of shader hashes play runs used (game-build/release_b/used_shaders.txt).
pub const USED_LIST_REL: &str = "shaders/used_shaders.txt";
/// In the game folder: the runtime's record of the shaders this install used ([nvn] shader_order, its default).
const ORDER_REL: &str = "shader_order.txt";
/// results.tsv column with the translator version that made the row (a row without it: version 0, before versioning).
const TRANSLATOR_COL: &str = "translator";

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Marker {
    /// romfs the corpus was extracted from
    pub romfs: String,
    pub extracted: bool,
    pub blocks: usize,
    pub complete: bool,
    /// translator version the finished cache (or its used part) was made with (0 = before versioning); `complete` and
    /// `ready` hold only for this version
    pub translator: u32,
    /// the used part is translated: Play may start while the rest is translated in the background
    pub ready: bool,
}

#[derive(Serialize, Clone, Default)]
pub struct Progress {
    /// "idle", "running", "done", "error", "cancelled"
    pub state: String,
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
    /// the shipped used-shader list; None (a tools folder without it): `Part::Used` translates everything, as `Part::All`
    pub used: Option<PathBuf>,
}

/// What a `run` translates.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Part {
    /// everything, used blocks first; Play waits for all of it (launcher option full_shader_setup)
    #[default]
    All,
    /// only the used blocks; the marker then says `ready`
    Used,
    /// what is left after `Used` (in the background); the marker then says `complete`
    Rest,
}

pub struct Job {
    pub game_dir: PathBuf,
    pub romfs: PathBuf,
    pub main: PathBuf,
    pub jobs: usize,
    /// Test runs: translate at most this many blocks (0 = all); the setup is then never marked complete.
    pub limit: usize,
    pub part: Part,
}

/// One writer of the cache at a time: `run` and `translate_missing` both append to results.tsv and use the stage folders.
fn lock_cache() -> MutexGuard<'static, ()> {
    static CACHE: Mutex<()> = Mutex::new(());
    CACHE.lock().unwrap_or_else(|e| e.into_inner())
}

/// Whether the background part may run. A hold (the game or the warm-up running, Set up) or the user's pause sets `stop`,
/// the running part's cancel flag: its harness is killed within 200 ms, the unfinished chunk is translated again later.
#[derive(Default)]
pub struct Background {
    pub stop: AtomicBool,
    ctl: Mutex<BgCtl>,
    /// the last progress of the background part
    pub progress: Mutex<Progress>,
}

#[derive(Default)]
struct BgCtl {
    holds: u32,
    paused: bool,
    running: bool,
}

/// Keeps the background part stopped while it lives.
pub struct Hold(Arc<Background>);

impl Drop for Hold {
    fn drop(&mut self) {
        let mut c = self.0.ctl();
        c.holds = c.holds.saturating_sub(1);
    }
}

impl Background {
    fn ctl(&self) -> MutexGuard<'_, BgCtl> {
        self.ctl.lock().unwrap_or_else(|e| e.into_inner())
    }
    /// Stops the background part (if it runs) and keeps it from starting until the returned Hold is dropped.
    pub fn hold(self: &Arc<Self>) -> Hold {
        let mut c = self.ctl();
        c.holds += 1;
        self.stop.store(true, Ordering::SeqCst);
        Hold(self.clone())
    }
    /// The user's pause (this launcher session only: the next launch continues).
    pub fn set_paused(&self, paused: bool) {
        let mut c = self.ctl();
        c.paused = paused;
        if paused {
            self.stop.store(true, Ordering::SeqCst);
        }
    }
    pub fn paused(&self) -> bool {
        self.ctl().paused
    }
    pub fn running(&self) -> bool {
        self.ctl().running
    }
    /// True when the caller may run the background part now (and calls `end` afterwards): nothing holds it, the user
    /// has not paused it, and no other run is going.
    pub fn begin(&self) -> bool {
        let mut c = self.ctl();
        if c.holds > 0 || c.paused || c.running {
            return false;
        }
        c.running = true;
        self.stop.store(false, Ordering::SeqCst);
        true
    }
    pub fn end(&self) {
        self.ctl().running = false;
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

fn harness_exe(tools_dir: &Path) -> PathBuf {
    tools_dir.join("spirv_harness").join("spirv_harness.exe")
}

/// The translator version of a harness (`spirv_harness --version`), asked once per process and harness. None: it cannot
/// be run, or it predates versioning (it then prints its usage and exits 2).
pub fn translator_version(harness: &Path) -> Option<u32> {
    static SEEN: OnceLock<Mutex<Vec<(PathBuf, Option<u32>)>>> = OnceLock::new();
    let seen = SEEN.get_or_init(Default::default);
    if let Some((_, v)) = seen.lock().unwrap().iter().find(|(p, _)| p == harness) {
        return *v;
    }
    let mut c = Command::new(harness);
    c.arg("--version").stdin(Stdio::null()).stderr(Stdio::null());
    quiet(&mut c);
    let v = c.output().ok().filter(|o| o.status.success()).and_then(|o| String::from_utf8_lossy(&o.stdout).trim().parse().ok());
    seen.lock().unwrap().push((harness.to_path_buf(), v));
    v
}

/// True when a finished setup's cache is in place and was made by the shipped translator. A harness that cannot report its
/// version does not judge the cache (the marker decides).
pub fn is_complete(game_dir: &Path, tools_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.complete && game_dir.join(SPIRV_REL).join("results.tsv").is_file() && !is_stale_marker(&m, tools_dir)
}

/// True when Play may start: the cache is finished, or its used part is (`ready`; the rest follows in the background),
/// by the shipped translator.
pub fn is_playable(game_dir: &Path, tools_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    (m.complete || m.ready) && game_dir.join(SPIRV_REL).join("results.tsv").is_file() && !is_stale_marker(&m, tools_dir)
}

/// True when the used part is done and the rest is not: the background part has work.
pub fn rest_pending(game_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.ready && !m.complete
}

/// True when the cache (or its used part) was made by another translator version than the shipped one: Set up
/// translates it again.
pub fn is_stale(game_dir: &Path, tools_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    (m.complete || m.ready) && is_stale_marker(&m, tools_dir)
}

fn is_stale_marker(m: &Marker, tools_dir: &Path) -> bool {
    translator_version(&harness_exe(tools_dir)).is_some_and(|v| v != m.translator)
}

/// Removes every results.tsv row (and its .spv) that another translator version made, so the usual "no row / no .spv =
/// translate it" rules pick those blocks up again. Returns the number of rows dropped.
/// The .spv files go first, then results.tsv is replaced through a temporary file: a crash in between leaves rows whose
/// .spv is gone, still stale, so the next call finishes the job; never a stale .spv without its row (translate_missing
/// would take it for a current translation).
pub fn drop_stale(spirv: &Path, version: u32) -> Result<usize, String> {
    let results = spirv.join("results.tsv");
    if !results.is_file() {
        return Ok(0);
    }
    let (header, rows) = read_tsv(&results)?;
    let k = header.split('\t').position(|h| h == TRANSLATOR_COL);
    let current = |r: &str| k.and_then(|k| r.split('\t').nth(k)).and_then(|x| x.parse::<u32>().ok()) == Some(version);
    let (keep, stale): (Vec<&String>, Vec<&String>) = rows.iter().partition(|r| current(r));
    if stale.is_empty() {
        return Ok(0);
    }
    for r in &stale {
        let f = spirv.join(format!("{}.spv", first_col(r)));
        if f.is_file() {
            fs::remove_file(&f).map_err(|e| format!("cannot remove {}: {}", f.display(), e))?;
        }
    }
    if keep.is_empty() {
        // no current row: the file goes, the next harness run writes its own header
        fs::remove_file(&results).map_err(|e| format!("cannot remove {}: {}", results.display(), e))?;
        return Ok(stale.len());
    }
    let mut text = String::with_capacity(header.len() + keep.iter().map(|r| r.len() + 1).sum::<usize>() + 1);
    text.push_str(&header);
    text.push('\n');
    for r in &keep {
        text.push_str(r);
        text.push('\n');
    }
    let tmp = spirv.join("results.tsv.tmp");
    fs::write(&tmp, text).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, &results).map_err(|e| format!("cannot replace {}: {}", results.display(), e))?;
    Ok(stale.len())
}

/// The harness's version; an error when it reports none (a tools folder from another release).
fn require_version(harness: &Path) -> Result<u32, String> {
    translator_version(harness).ok_or_else(|| {
        format!("the shader translator ({}) did not report its version. Re-extract the launcher's release folder so tools/ is complete.", harness.display())
    })
}

fn quiet(cmd: &mut Command) {
    quiet_at(cmd, false);
}

fn quiet_at(cmd: &mut Command, idle: bool) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, or IDLE_PRIORITY_CLASS for the background part (the tools' own
        // child processes inherit it)
        cmd.creation_flags(0x0800_0000 | if idle { 0x0000_0040 } else { 0x0000_4000 });
    }
    #[cfg(not(windows))]
    let _ = (cmd, idle);
}

fn python_has_lz4(py: &Path) -> bool {
    let mut c = Command::new(py);
    c.args(["-I", "-c", "import lz4.block"]).stdout(Stdio::null()).stderr(Stdio::null());
    quiet(&mut c);
    c.status().map(|s| s.success()).unwrap_or(false)
}

fn on_path(exe: &str) -> Vec<PathBuf> {
    std::env::var_os("PATH").map(|p| std::env::split_paths(&p).map(|d| d.join(exe)).filter(|f| f.is_file()).collect()).unwrap_or_default()
}

/// Finds the tools in `tools_dir` (the launcher's `tools/` folder). Python: `tools/python/python.exe`, else the first
/// python.exe on PATH that has `lz4`. Every missing piece is named in the error.
pub fn find_tools(tools_dir: &Path) -> Result<Tools, String> {
    let mut missing = Vec::new();
    let harness = harness_exe(tools_dir);
    if !harness.is_file() {
        missing.push(format!("the shader translator ({})", harness.display()));
    }
    let extract = tools_dir.join("shaders").join("extract_corpus.py");
    if !extract.is_file() {
        missing.push(format!("the shader extractor ({})", extract.display()));
    }
    let bundled = tools_dir.join("python").join("python.exe");
    let mut candidates = vec![bundled.clone()];
    candidates.extend(on_path("python.exe"));
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
    let used = Some(tools_dir.join(USED_LIST_REL)).filter(|p| p.is_file());
    Ok(Tools { python: python.unwrap(), extract, harness, used })
}

/// The hashes of a list file in file order, each once: one per line, '#' lines and anything that is not 16 lowercase
/// hex skipped. A missing file is an empty list.
pub fn read_hash_list(p: &Path) -> Vec<String> {
    let text = fs::read_to_string(p).unwrap_or_default();
    let mut seen = HashSet::new();
    text.lines()
        .map(|l| l.trim())
        .filter(|h| h.len() == 16 && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)))
        .filter(|h| seen.insert(h.to_string()))
        .map(String::from)
        .collect()
}

/// The blocks translated first: the shipped list, then the shaders this install used. None without a shipped list.
fn used_hashes(game_dir: &Path, list: Option<&Path>) -> Option<Vec<String>> {
    let mut used = read_hash_list(list?);
    let known: HashSet<String> = used.iter().cloned().collect();
    used.extend(read_hash_list(&game_dir.join(ORDER_REL)).into_iter().filter(|h| !known.contains(h)));
    Some(used)
}

/// The corpus rows still to translate, as (used, rest): the used ones in the order of `used`, the rest in corpus order.
/// A used hash the corpus does not have (a block the game builds while it runs) is left to translate_missing.
fn split_todo<'a>(rows: &'a [String], done: &HashSet<String>, used: &[String]) -> (Vec<&'a String>, Vec<&'a String>) {
    let todo: HashMap<&str, &'a String> = rows.iter().map(|r| (first_col(r), r)).filter(|(h, _)| !done.contains(*h)).collect();
    let mut taken: HashSet<&str> = HashSet::new();
    let first: Vec<&'a String> = used.iter().filter_map(|h| todo.get(h.as_str()).copied()).filter(|r| taken.insert(first_col(r))).collect();
    let rest = rows.iter().filter(|r| todo.contains_key(first_col(r)) && !taken.contains(first_col(r))).collect();
    (first, rest)
}

/// Runs a tool, handing each stdout line to `line`; kills it when `cancel` is set. Ok(true) = exit 0, Ok(false) = cancelled.
fn run_tool(cmd: Command, cancel: &AtomicBool, line: impl FnMut(&str)) -> Result<bool, String> {
    run_tool_at(cmd, false, cancel, line)
}

fn run_tool_at(mut cmd: Command, idle: bool, cancel: &AtomicBool, mut line: impl FnMut(&str)) -> Result<bool, String> {
    quiet_at(&mut cmd, idle);
    let name = cmd.get_program().to_string_lossy().to_string();
    let mut child: Child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start {}: {}", name, e))?;
    let (tx, rx) = mpsc::channel::<String>();
    let out = child.stdout.take().unwrap();
    let tx2 = tx.clone();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx2.send(l))));
    let err = child.stderr.take().unwrap();
    let (etx, erx) = mpsc::channel::<String>();
    std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        let _ = etx.send(tail.join("\n"));
    });
    drop(tx);
    loop {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => line(&l),
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                // stdout closed: the process is ending
                let st = child.wait().map_err(|e| e.to_string())?;
                if cancel.load(Ordering::SeqCst) {
                    return Ok(false);
                }
                if st.success() {
                    return Ok(true);
                }
                let tail = erx.recv_timeout(Duration::from_secs(5)).unwrap_or_default();
                return Err(format!("{} failed ({}). {}", name, st, tail.trim()));
            }
        }
        if cancel.load(Ordering::SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            return Ok(false);
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

/// Moves a harness run's .spv files from `stage` into `spirv`, then appends its rows to spirv/results.tsv (the rows are
/// what make a block count as done, so they go last). Returns the number of rows.
fn commit_stage(stage: &Path, spirv: &Path) -> Result<usize, String> {
    let results = spirv.join("results.tsv");
    let (rheader, rrows) = read_tsv(&stage.join("results.tsv"))?;
    for r in &rrows {
        let h = first_col(r);
        let src = stage.join(format!("{}.spv", h));
        if src.is_file() {
            let dst = spirv.join(format!("{}.spv", h));
            let _ = fs::remove_file(&dst);
            fs::rename(&src, &dst).map_err(|e| format!("cannot move {}: {}", src.display(), e))?;
        }
    }
    let new_file = !results.is_file();
    let mut out = fs::OpenOptions::new().create(true).append(true).open(&results).map_err(|e| format!("cannot write {}: {}", results.display(), e))?;
    let mut text = String::new();
    if new_file {
        text.push_str(&rheader);
        text.push('\n');
    }
    for r in &rrows {
        text.push_str(r);
        text.push('\n');
    }
    out.write_all(text.as_bytes()).map_err(|e| format!("cannot write {}: {}", results.display(), e))?;
    Ok(rrows.len())
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
    let _lock = lock_cache();
    // a cache of another translator: its run-time blocks are made again here (the romfs ones by Set up, which is_complete asks for)
    drop_stale(&spirv, require_version(&harness_exe(tools_dir))?)?;
    // ponytail: a block the translator fails on has no .spv and is tried again at every start (a second or two)
    let todo: Vec<&String> = rows.iter().filter(|r| !spirv.join(format!("{}.spv", col(r, 10))).is_file()).collect();
    if todo.is_empty() {
        return Ok(0);
    }
    // the harness reads the code and control files relative to its manifest: absolute paths point it at RUNTIME_REL
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
    let jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2).to_string();
    let mut cmd = Command::new(harness_exe(tools_dir));
    cmd.arg("--batch").arg(&input).arg("--out").arg(&stage).args(["--jobs", &jobs, "--no-val"]);
    cmd.env("DOTNET_gcServer", "1");
    if !run_tool(cmd, &AtomicBool::new(false), |_| {})? {
        return Err("the shader translator stopped".into());
    }
    let n = commit_stage(&stage, &spirv)?;
    let _ = fs::remove_dir_all(&stage);
    let _ = fs::remove_dir_all(&input);
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
    let mut cmd = Command::new(game_dir.join("pkp.exe"));
    cmd.current_dir(game_dir).arg("--headless").args(["--max-seconds", &WARMUP_SECONDS.to_string()]).arg("--log").arg(scratch.join("warmup.log"));
    // environment variables win over pkp.ini
    cmd.env("NXRT_FS_SAVE_DIR", scratch.join("save")).env("NXRT_NVN_DUMP_MISSING", MISSING_REL).env("NXRT_AUDIO_SINK", "null");
    // it ends with exit code 124 when the time is up: any end is fine, a crash only means fewer blocks were collected
    if let Ok(false) = run_tool(cmd, cancel, |_| {}) {
        return Ok(None);
    }
    let n = translate_missing(game_dir, tools_dir)?;
    // an empty list still records that the warm-up ran: a run that collects nothing is not repeated at every start
    let list = game_dir.join(RUNTIME_REL).join("manifest.tsv");
    if !list.is_file() {
        let _ = fs::create_dir_all(game_dir.join(RUNTIME_REL));
        fs::write(&list, "").map_err(|e| format!("cannot write {}: {}", list.display(), e))?;
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

/// What a run translates, in order: the used blocks still to do (`first`), then the others (`rest`).
struct Plan<'a> {
    first: Vec<&'a String>,
    rest: Vec<&'a String>,
    /// blocks not done before the limit cut the lists
    left: usize,
    /// the limit left blocks out
    limited: bool,
    /// the limit left no used block out: once `first` is done, the used part is
    first_whole: bool,
}

/// `used` None (no shipped list): nothing is known to be used, so the used part is everything and Play waits for all.
fn plan<'a>(rows: &'a [String], done: &HashSet<String>, used: Option<&[String]>, limit: usize) -> Plan<'a> {
    let (mut first, mut rest) = split_todo(rows, done, used.unwrap_or(&[]));
    if used.is_none() {
        first.append(&mut rest);
    }
    let left = first.len() + rest.len();
    let limited = limit > 0 && left > limit;
    let first_whole = !(limit > 0 && first.len() > limit);
    if limit > 0 {
        first.truncate(limit);
        rest.truncate(limit - first.len());
    }
    Plan { first, rest, left, limited, first_whole }
}

/// One harness setup for `translate_rows`.
struct Batch<'a> {
    header: &'a str,
    corpus: &'a Path,
    stage: &'a Path,
    spirv: &'a Path,
    harness: &'a Path,
    jobs: usize,
    chunk: usize,
    /// the background part: idle priority, and the .NET server GC with one heap per worker instead of one per core
    idle: bool,
}

/// Translates `rows` (corpus rows) chunk by chunk; each chunk counts once committed. `tick` gets the number of rows of
/// `rows` done so far. Ok(false) = cancelled.
fn translate_rows(b: &Batch, rows: &[&String], cancel: &AtomicBool, mut tick: impl FnMut(u64)) -> Result<bool, String> {
    let mut done = 0u64;
    for chunk in rows.chunks(b.chunk.max(1)) {
        if cancel.load(Ordering::SeqCst) {
            return Ok(false);
        }
        let mut f = fs::File::create(b.corpus.join("unique.tsv")).map_err(|e| e.to_string())?;
        writeln!(f, "{}", b.header).map_err(|e| e.to_string())?;
        for r in chunk {
            writeln!(f, "{}", r).map_err(|e| e.to_string())?;
        }
        drop(f);
        let _ = fs::remove_dir_all(b.stage);
        let jobs = b.jobs.max(1);
        let mut cmd = Command::new(b.harness);
        cmd.arg("--batch").arg(b.corpus).arg("--out").arg(b.stage).args(["--jobs", &jobs.to_string(), "--no-val"]);
        // server GC: with the default workstation GC each collection stops every worker (about 4x slower)
        cmd.env("DOTNET_gcServer", "1");
        if b.idle {
            cmd.env("DOTNET_GCHeapCount", format!("{:x}", jobs));
        }
        let base = done;
        let ok = run_tool_at(cmd, b.idle, cancel, |l| {
            // "  <n>/<count>  <s> s" every 500 blocks
            if let Some(n) = l.trim().split('/').next().and_then(|x| x.parse::<u64>().ok()) {
                tick(base + n);
            }
        })?;
        if !ok {
            return Ok(false);
        }
        commit_stage(b.stage, b.spirv)?;
        done += chunk.len() as u64;
        tick(done);
    }
    Ok(true)
}

/// The setup's shader steps for `job.part` (see Part). `report` gets every progress change. Ok(true) = that part is
/// finished (or the limit reached), Ok(false) = cancelled.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Progress)) -> Result<bool, String> {
    let _lock = lock_cache();
    if cancel.load(Ordering::SeqCst) {
        return Ok(false);
    }
    let cache = cache_dir(&job.game_dir);
    let corpus = cache.join("corpus");
    let stage = cache.join("stage");
    let spirv = job.game_dir.join(SPIRV_REL);
    fs::create_dir_all(&spirv).map_err(|e| format!("cannot create {}: {}", spirv.display(), e))?;
    let romfs_s = job.romfs.to_string_lossy().replace('\\', "/");
    let mut marker = read_marker(&job.game_dir);
    if marker.romfs != romfs_s {
        if job.part == Part::Rest {
            return Err("The game files changed since the shaders were set up: press Set up again (Game files).".into());
        }
        marker = Marker { romfs: romfs_s.clone(), ..Default::default() };
    }
    let version = require_version(&tools.harness)?;
    if marker.translator != version {
        marker.ready = false;
    }
    marker.complete = false;
    write_marker(&job.game_dir, &marker)?;
    // rows of another translator version go first, so step 2 counts their blocks as not done
    drop_stale(&spirv, version)?;
    let background = job.part == Part::Rest;
    let jobs = job.jobs.max(1).to_string();
    let p = |phase: &str, done: u64, total: u64, message: String| Progress { state: "running".into(), phase: phase.into(), done, total, message };

    // 1. extract (skipped when this romfs was already extracted)
    let all = corpus.join("unique_all.tsv");
    if !(marker.extracted && all.is_file()) {
        report(p("extract", 0, 0, "Finding the game's shaders in your game files...".into()));
        let _ = fs::remove_file(corpus.join("unique.tsv"));
        let mut cmd = Command::new(&tools.python);
        cmd.arg("-I").arg(&tools.extract).arg("--romfs").arg(&job.romfs).arg("--main").arg(&job.main).arg("--out").arg(&corpus);
        cmd.args(["--jobs", &jobs, "--progress"]);
        let ok = run_tool_at(cmd, background, cancel, |l| {
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

    // 2. translate what spirv/results.tsv does not list yet: the used blocks first, then the rest
    let (uheader, urows) = read_tsv(&all)?;
    marker.blocks = urows.len();
    write_marker(&job.game_dir, &marker)?;
    let results = spirv.join("results.tsv");
    let done: HashSet<String> = if results.is_file() { read_tsv(&results)?.1.iter().map(|r| first_col(r).to_string()).collect() } else { HashSet::new() };
    let used = used_hashes(&job.game_dir, tools.used.as_deref());
    let Plan { first, rest, left, limited, first_whole } = plan(&urows, &done, used.as_deref(), job.limit);
    let total = urows.len() as u64;
    let mut finished = total - left as u64;
    let batch = Batch {
        header: &uheader,
        corpus: &corpus,
        stage: &stage,
        spirv: &spirv,
        harness: &tools.harness,
        jobs: job.jobs,
        chunk: if background { BG_CHUNK } else { CHUNK },
        idle: background,
    };
    let clean = || {
        let _ = fs::remove_dir_all(&stage);
        let _ = fs::remove_file(corpus.join("unique.tsv"));
    };
    if !first.is_empty() {
        // Part::Used counts only the used blocks: that is what Play waits for
        let (base, of) = if job.part == Part::Used { (0, first.len() as u64) } else { (finished, total) };
        let msg = |d: u64| {
            if job.part == Part::Used {
                format!("Translating the shaders the game uses: {} of {}", d, of)
            } else {
                format!("Translating shaders: {} of {}", d, of)
            }
        };
        report(p("translate", base, of, msg(base)));
        if !translate_rows(&batch, &first, cancel, |d| report(p("translate", base + d, of, msg(base + d))))? {
            return Ok(false);
        }
        finished += first.len() as u64;
    }
    if first_whole && !marker.ready {
        marker.ready = true;
        marker.translator = version;
        write_marker(&job.game_dir, &marker)?;
    }
    if job.part == Part::Used && (!rest.is_empty() || limited) {
        clean();
        return Ok(true);
    }
    if !rest.is_empty() {
        let what = if background { "Translating the other shaders in the background" } else { "Translating shaders" };
        let base = finished;
        report(p("translate", base, total, format!("{}: {} of {}", what, base, total)));
        if !translate_rows(&batch, &rest, cancel, |d| report(p("translate", base + d, total, format!("{}: {} of {}", what, base + d, total))))? {
            return Ok(false);
        }
    }
    clean();
    if limited {
        return Ok(true);
    }

    // 3. done: the corpus is only needed to translate, drop it (several GB)
    report(p("finish", total, total, "Cleaning up...".into()));
    let _ = fs::remove_dir_all(&corpus);
    marker.extracted = false;
    marker.complete = true;
    marker.ready = true;
    marker.translator = version;
    write_marker(&job.game_dir, &marker)?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-shaders-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    const HEADER: &str = "hash\tgroup\tstage\tstages_using\tstatus\tms\tspv_bytes\tresources\ttranslator_messages\terror\tdraw_time_queries\tglslc_reflection\ttranslator";

    fn row(h: &str, v: u32) -> String {
        format!("{}\tps\tfragment\t1\tok\t1.0\t4\tcb0=s0b1\t\t\t\t\t{}", h, v)
    }

    #[test]
    fn unversioned_cache_is_dropped_whole() {
        // results.tsv of a harness before versioning: no translator column, every row is version 0
        let d = temp("unversioned");
        fs::write(d.join("results.tsv"), "hash\tgroup\tstage\tresources\naaaa\tps\tfragment\tcb0=s0b1\n").unwrap();
        fs::write(d.join("aaaa.spv"), b"spv").unwrap();
        assert_eq!(drop_stale(&d, 1).unwrap(), 1);
        assert!(!d.join("results.tsv").exists() && !d.join("aaaa.spv").exists());
        assert_eq!(drop_stale(&d, 1).unwrap(), 0);
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn only_other_versions_are_dropped() {
        let d = temp("mixed");
        fs::write(d.join("results.tsv"), format!("{}\n{}\n{}\n{}\n", HEADER, row("aaaa", 1), row("bbbb", 2), row("cccc", 3))).unwrap();
        for h in ["aaaa", "bbbb", "cccc"] {
            fs::write(d.join(format!("{}.spv", h)), b"spv").unwrap();
        }
        assert_eq!(drop_stale(&d, 2).unwrap(), 2);
        let (header, rows) = read_tsv(&d.join("results.tsv")).unwrap();
        assert_eq!(header, HEADER);
        assert_eq!(rows, vec![row("bbbb", 2)]);
        assert!(d.join("bbbb.spv").is_file() && !d.join("aaaa.spv").exists() && !d.join("cccc.spv").exists());
        assert!(!d.join("results.tsv.tmp").exists());
        // a current cache is left as it is
        assert_eq!(drop_stale(&d, 2).unwrap(), 0);
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn marker_without_version_reads_as_zero() {
        let d = temp("marker");
        fs::create_dir_all(d.join(SPIRV_REL)).unwrap();
        fs::write(d.join(SPIRV_REL).join("results.tsv"), format!("{}\n", HEADER)).unwrap();
        // setup.json of a launcher before versioning
        fs::write(cache_dir(&d).join("setup.json"), "{\"romfs\": \"x\", \"extracted\": false, \"blocks\": 3, \"complete\": true}").unwrap();
        assert_eq!(read_marker(&d).translator, 0);
        // no harness in this tools folder: it cannot report a version, so the marker decides
        assert!(is_complete(&d, &d.join("tools")) && !is_stale(&d, &d.join("tools")));
        fs::remove_dir_all(&d).unwrap();
    }

    fn hashes(rows: &[&String]) -> Vec<String> {
        rows.iter().map(|r| first_col(r).to_string()).collect()
    }

    fn corpus_row(h: &str) -> String {
        format!("{}\tps\t0x100\t0xd8\t1\t1\t0x38f972af.fdata\t0x0\t0x0\tc\tg", h)
    }

    fn hex(i: usize) -> String {
        format!("{:016x}", 0xa000 + i)
    }

    #[test]
    fn hash_list_skips_comments_junk_and_repeats() {
        let d = temp("list");
        let a = hex(1);
        let b = hex(2);
        fs::write(d.join("l.txt"), format!("# header\n{a}\n\n  {b}  \r\nnot-a-hash\n{a}\nABCDEF0123456789\n0123\n")).unwrap();
        assert_eq!(read_hash_list(&d.join("l.txt")), vec![a, b]);
        assert!(read_hash_list(&d.join("missing.txt")).is_empty());
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn used_blocks_go_first_in_list_order() {
        let rows: Vec<String> = (0..6).map(|i| corpus_row(&hex(i))).collect();
        let done: HashSet<String> = [hex(1)].into_iter().collect();
        // a used hash twice, one the corpus lacks (a run-time block), and one already done
        let used = vec![hex(4), "ffffffffffffffff".into(), hex(1), hex(2), hex(4)];
        let p = plan(&rows, &done, Some(&used), 0);
        assert_eq!(hashes(&p.first), vec![hex(4), hex(2)]);
        assert_eq!(hashes(&p.rest), vec![hex(0), hex(3), hex(5)]);
        assert_eq!(p.left, 5);
        assert!(!p.limited && p.first_whole);
    }

    #[test]
    fn without_a_list_everything_is_the_used_part() {
        let rows: Vec<String> = (0..3).map(|i| corpus_row(&hex(i))).collect();
        let p = plan(&rows, &HashSet::new(), None, 0);
        assert_eq!(hashes(&p.first), vec![hex(0), hex(1), hex(2)]);
        assert!(p.rest.is_empty());
        // an empty shipped list is still a list: nothing is used, all of it is the rest
        let p = plan(&rows, &HashSet::new(), Some(&[]), 0);
        assert!(p.first.is_empty() && p.rest.len() == 3);
    }

    #[test]
    fn limit_cuts_used_first() {
        let rows: Vec<String> = (0..6).map(|i| corpus_row(&hex(i))).collect();
        let used = vec![hex(5), hex(3)];
        let p = plan(&rows, &HashSet::new(), Some(&used), 3);
        assert_eq!(hashes(&p.first), vec![hex(5), hex(3)]);
        assert_eq!(hashes(&p.rest), vec![hex(0)]);
        assert!(p.limited && p.first_whole);
        let p = plan(&rows, &HashSet::new(), Some(&used), 1);
        assert_eq!(hashes(&p.first), vec![hex(5)]);
        assert!(p.rest.is_empty() && p.limited && !p.first_whole);
    }

    #[test]
    fn plan_resumes_where_a_stopped_run_left_off() {
        // a background run killed after its first chunk: the rows it committed are done, the next plan has the same
        // order without them
        let rows: Vec<String> = (0..8).map(|i| corpus_row(&hex(i))).collect();
        let used = vec![hex(6)];
        let p = plan(&rows, &HashSet::new(), Some(&used), 0);
        let order: Vec<String> = hashes(&p.first).into_iter().chain(hashes(&p.rest)).collect();
        let done: HashSet<String> = order[..3].iter().cloned().collect();
        let p = plan(&rows, &done, Some(&used), 0);
        assert!(p.first.is_empty());
        assert_eq!(hashes(&p.rest), order[3..].to_vec());
        // a block the game used meanwhile (shader_order.txt) jumps the queue
        let used = vec![hex(6), hex(7)];
        let p = plan(&rows, &done, Some(&used), 0);
        assert_eq!(hashes(&p.first), vec![hex(7)]);
        assert_eq!(p.rest.len(), 4);
    }

    #[test]
    fn used_list_adds_this_installs_order_file() {
        let d = temp("used");
        fs::write(d.join("list.txt"), format!("# shipped\n{}\n{}\n", hex(1), hex(2))).unwrap();
        assert!(used_hashes(&d, None).is_none());
        assert_eq!(used_hashes(&d, Some(&d.join("list.txt"))).unwrap(), vec![hex(1), hex(2)]);
        fs::write(d.join(ORDER_REL), format!("{}\n{}\n", hex(2), hex(3))).unwrap();
        assert_eq!(used_hashes(&d, Some(&d.join("list.txt"))).unwrap(), vec![hex(1), hex(2), hex(3)]);
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn background_stops_for_holds_and_pause() {
        let bg = Arc::new(Background::default());
        assert!(bg.begin());
        assert!(!bg.begin(), "one run at a time");
        assert!(!bg.stop.load(Ordering::SeqCst));
        // the game starts: the running part is told to stop, and none starts while the game runs
        let game = bg.hold();
        assert!(bg.stop.load(Ordering::SeqCst));
        bg.end();
        assert!(!bg.begin());
        let setup = bg.hold();
        drop(game);
        assert!(!bg.begin(), "a second hold still holds");
        drop(setup);
        assert!(bg.begin() && !bg.stop.load(Ordering::SeqCst));
        bg.set_paused(true);
        assert!(bg.stop.load(Ordering::SeqCst) && bg.paused());
        bg.end();
        assert!(!bg.begin());
        bg.set_paused(false);
        assert!(bg.begin());
        bg.end();
        assert!(!bg.running());
    }

    #[test]
    fn ready_marker_lets_play_start() {
        let d = temp("ready");
        fs::create_dir_all(d.join(SPIRV_REL)).unwrap();
        fs::write(d.join(SPIRV_REL).join("results.tsv"), format!("{}\n", HEADER)).unwrap();
        let tools = d.join("tools");
        write_marker(&d, &Marker { romfs: "x".into(), extracted: true, blocks: 9, ready: true, translator: 1, ..Default::default() }).unwrap();
        assert!(is_playable(&d, &tools) && !is_complete(&d, &tools) && rest_pending(&d) && !is_stale(&d, &tools));
        write_marker(&d, &Marker { romfs: "x".into(), blocks: 9, ready: true, complete: true, translator: 1, ..Default::default() }).unwrap();
        assert!(is_playable(&d, &tools) && is_complete(&d, &tools) && !rest_pending(&d));
        write_marker(&d, &Marker { romfs: "x".into(), extracted: true, blocks: 9, ..Default::default() }).unwrap();
        assert!(!is_playable(&d, &tools) && !rest_pending(&d));
        fs::remove_dir_all(&d).unwrap();
    }

    /// A stand-in harness: `--version` prints 1; `--batch <corpus> --out <stage>` writes a row and a .spv for every
    /// unique.tsv row, and appends the hashes it got to <corpus>/../calls.txt (one line per run).
    #[cfg(windows)]
    fn fake_tools(d: &Path) -> Tools {
        let ps = r#"$a = $args
if ($a[0] -eq '--version') { '1'; exit 0 }
$corpus = $a[1]; $out = $a[3]
New-Item -ItemType Directory -Force $out | Out-Null
$hs = @(Get-Content (Join-Path $corpus 'unique.tsv') | Select-Object -Skip 1 | ForEach-Object { $_.Split("`t")[0] })
$lines = @("hash`tgroup`tstage`tstages_using`tstatus`tms`tspv_bytes`tresources`ttranslator_messages`terror`tdraw_time_queries`tglslc_reflection`ttranslator")
foreach ($h in $hs) { Set-Content (Join-Path $out "$h.spv") 'spv'; $lines += "$h`tps`tfragment`t1`tok`t1.0`t4`tcb0=s0b1`t`t`t`t`t1" }
$lines | Set-Content (Join-Path $out 'results.tsv')
Add-Content (Join-Path $corpus '..\calls.txt') ($hs -join ' ')
"#;
        fs::write(d.join("fake.ps1"), ps).unwrap();
        fs::write(d.join("fake.cmd"), "@powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0fake.ps1\" %*\r\n").unwrap();
        Tools { python: d.join("none"), extract: d.join("none"), harness: d.join("fake.cmd"), used: Some(d.join("used.txt")) }
    }

    #[cfg(windows)]
    #[test]
    fn used_part_then_rest_in_background() {
        let d = temp("parts");
        let tools = fake_tools(&d);
        let game = d.join("game");
        let corpus = cache_dir(&game).join("corpus");
        fs::create_dir_all(&corpus).unwrap();
        let header = "code_sha1_16\tstage\tcode_size\tctrl_size\toccurrences\tctrl_variants\tfirst_source\tentry_off\tglslc_off\tfirst_ctrl\tfirst_glslc";
        let rows: String = (0..5).map(|i| corpus_row(&hex(i)) + "\n").collect();
        fs::write(corpus.join("unique_all.tsv"), format!("{}\n{}", header, rows)).unwrap();
        fs::write(d.join("used.txt"), format!("{}\n{}\n", hex(3), hex(1))).unwrap();
        write_marker(&game, &Marker { romfs: "r".into(), extracted: true, ..Default::default() }).unwrap();
        let mut job = Job { game_dir: game.clone(), romfs: PathBuf::from("r"), main: d.join("main"), jobs: 1, limit: 0, part: Part::Used };
        let calls = || fs::read_to_string(cache_dir(&game).join("calls.txt")).unwrap_or_default();

        // the used part: only the listed blocks, in list order; Play may start
        assert!(run(&job, &tools, &AtomicBool::new(false), |_| {}).unwrap());
        assert_eq!(calls().trim(), format!("{} {}", hex(3), hex(1)));
        assert!(read_marker(&game).ready && rest_pending(&game) && is_playable(&game, &d));
        assert!(game.join(SPIRV_REL).join(format!("{}.spv", hex(3))).is_file());

        // the background part, stopped before it starts: nothing runs, nothing is lost
        job.part = Part::Rest;
        assert!(!run(&job, &tools, &AtomicBool::new(true), |_| {}).unwrap());
        assert_eq!(calls().lines().count(), 1);
        assert!(read_marker(&game).ready && rest_pending(&game));

        // then to the end: the rest in corpus order, the cache complete, the corpus gone
        assert!(run(&job, &tools, &AtomicBool::new(false), |_| {}).unwrap());
        assert_eq!(calls().lines().nth(1).unwrap().trim(), format!("{} {} {}", hex(0), hex(2), hex(4)));
        let m = read_marker(&game);
        assert!(m.complete && m.ready && m.translator == 1 && !rest_pending(&game) && !corpus.exists());
        let (_, rows) = read_tsv(&game.join(SPIRV_REL).join("results.tsv")).unwrap();
        assert_eq!(rows.len(), 5);
        fs::remove_dir_all(&d).unwrap();
    }
}
