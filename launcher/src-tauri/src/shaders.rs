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
//! No crate-local imports, so `examples/shader_setup.rs` can build this file alone.

use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Mutex, OnceLock};
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
    /// translator version the finished cache was made with (0 = before versioning); `complete` holds only for this version
    pub translator: u32,
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
}

pub struct Job {
    pub game_dir: PathBuf,
    pub romfs: PathBuf,
    pub main: PathBuf,
    pub jobs: usize,
    /// Test runs: translate at most this many blocks (0 = all); the setup is then never marked complete.
    pub limit: usize,
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

/// True when the cache was finished by another translator version than the shipped one: Set up translates it again.
pub fn is_stale(game_dir: &Path, tools_dir: &Path) -> bool {
    let m = read_marker(game_dir);
    m.complete && is_stale_marker(&m, tools_dir)
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
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS (the tools' own child processes inherit it)
        cmd.creation_flags(0x0800_0000 | 0x0000_4000);
    }
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
    Ok(Tools { python: python.unwrap(), extract, harness })
}

/// Runs a tool, handing each stdout line to `line`; kills it when `cancel` is set. Ok(true) = exit 0, Ok(false) = cancelled.
fn run_tool(mut cmd: Command, cancel: &AtomicBool, mut line: impl FnMut(&str)) -> Result<bool, String> {
    quiet(&mut cmd);
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

/// The whole setup. `report` gets every progress change. Ok(true) = finished (or the limit reached), Ok(false) = cancelled.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Progress)) -> Result<bool, String> {
    let cache = cache_dir(&job.game_dir);
    let corpus = cache.join("corpus");
    let stage = cache.join("stage");
    let spirv = job.game_dir.join(SPIRV_REL);
    fs::create_dir_all(&spirv).map_err(|e| format!("cannot create {}: {}", spirv.display(), e))?;
    let romfs_s = job.romfs.to_string_lossy().replace('\\', "/");
    let mut marker = read_marker(&job.game_dir);
    if marker.romfs != romfs_s {
        marker = Marker { romfs: romfs_s.clone(), ..Default::default() };
    }
    marker.complete = false;
    write_marker(&job.game_dir, &marker)?;
    // rows of another translator version go first, so step 2 counts their blocks as not done
    let version = require_version(&tools.harness)?;
    drop_stale(&spirv, version)?;
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
        let ok = run_tool(cmd, cancel, |l| {
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
    let done: HashSet<String> = if results.is_file() { read_tsv(&results)?.1.iter().map(|r| first_col(r).to_string()).collect() } else { HashSet::new() };
    let mut todo: Vec<&String> = urows.iter().filter(|r| !done.contains(first_col(r))).collect();
    let limited = job.limit > 0 && todo.len() > job.limit;
    if job.limit > 0 {
        todo.truncate(job.limit);
    }
    let total = urows.len() as u64;
    let mut finished = (urows.len() - urows.iter().filter(|r| !done.contains(first_col(r))).count()) as u64;
    report(p("translate", finished, total, format!("Translating shaders: {} of {}", finished, total)));
    for chunk in todo.chunks(CHUNK) {
        let mut f = fs::File::create(corpus.join("unique.tsv")).map_err(|e| e.to_string())?;
        writeln!(f, "{}", uheader).map_err(|e| e.to_string())?;
        for r in chunk {
            writeln!(f, "{}", r).map_err(|e| e.to_string())?;
        }
        drop(f);
        let _ = fs::remove_dir_all(&stage);
        let mut cmd = Command::new(&tools.harness);
        cmd.arg("--batch").arg(&corpus).arg("--out").arg(&stage).args(["--jobs", &jobs, "--no-val"]);
        // server GC: with the default workstation GC each collection stops every worker (about 4x slower)
        cmd.env("DOTNET_gcServer", "1");
        let base = finished;
        let ok = run_tool(cmd, cancel, |l| {
            // "  <n>/<count>  <s> s" every 500 blocks
            if let Some(n) = l.trim().split('/').next().and_then(|x| x.parse::<u64>().ok()) {
                report(p("translate", base + n, total, format!("Translating shaders: {} of {}", base + n, total)));
            }
        })?;
        if !ok {
            return Ok(false);
        }
        commit_stage(&stage, &spirv)?;
        finished += chunk.len() as u64;
        report(p("translate", finished, total, format!("Translating shaders: {} of {}", finished, total)));
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
}
