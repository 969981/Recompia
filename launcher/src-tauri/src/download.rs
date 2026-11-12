// SPDX-License-Identifier: GPL-3.0-or-later
//! Downloads checked against the size and SHA-256 their publisher lists for them: the Linux build tools (host.rs) and mods from the
//! Modtopia catalog (mods_install.rs). A file that does not match is never kept, and a finished one appears under its name at once
//! (written as `<file>.part`, then renamed), so nothing ever reads half a download.

use std::io::{Read, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

/// One download: the file name it gets, where it comes from, and what it must be.
pub struct Pinned<'a> {
    pub file: &'a str,
    pub url: &'a str,
    pub size: u64,
    /// lower-case hex
    pub sha256: &'a str,
}

/// The launcher's HTTP agent for downloads. `redirects`: false refuses an answer that sends it elsewhere, for a download that must come
/// from one host only.
pub fn agent(redirects: bool) -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(20))
        .timeout_read(Duration::from_secs(60))
        .redirects(if redirects { 5 } else { 0 })
        .user_agent(&format!("PokopiaRecomp-Launcher/{}", env!("CARGO_PKG_VERSION")))
        .build()
}

fn hex(h: &[u8]) -> String {
    h.iter().map(|b| format!("{:02x}", b)).collect()
}

/// Downloads `p` into `dir` unless a file that matches it is there. Ok(false) = cancelled. `progress` gets the bytes so far.
pub fn fetch(agent: &ureq::Agent, p: &Pinned, dir: &Path, cancel: &AtomicBool, progress: &mut dyn FnMut(u64)) -> Result<bool, String> {
    use sha2::{Digest, Sha256};
    let dst = dir.join(p.file);
    let want = p.sha256.to_ascii_lowercase();
    if std::fs::read(&dst).map(|b| b.len() as u64 == p.size && hex(&Sha256::digest(&b)) == want).unwrap_or(false) {
        progress(p.size);
        return Ok(true);
    }
    let part = dir.join(format!("{}.part", p.file));
    let resp = agent.get(p.url).call().map_err(|e| format!("Could not download {}: {}", p.url, e))?;
    let mut body = resp.into_reader().take(p.size + 1);
    let mut f = std::fs::File::create(&part).map_err(|e| format!("cannot write {}: {}", part.display(), e))?;
    let (mut h, mut n, mut buf) = (Sha256::new(), 0u64, vec![0u8; 1 << 16]);
    loop {
        if cancel.load(Ordering::SeqCst) {
            drop(f);
            let _ = std::fs::remove_file(&part);
            return Ok(false);
        }
        let k = match body.read(&mut buf) {
            Ok(k) => k,
            Err(e) => {
                drop(f);
                let _ = std::fs::remove_file(&part);
                return Err(format!("Downloading {} failed: {}", p.url, e));
            }
        };
        if k == 0 {
            break;
        }
        h.update(&buf[..k]);
        if let Err(e) = f.write_all(&buf[..k]) {
            drop(f);
            let _ = std::fs::remove_file(&part);
            return Err(format!("cannot write {}: {}", part.display(), e));
        }
        n += k as u64;
        progress(n);
    }
    drop(f);
    let got = hex(&h.finalize());
    if n != p.size || got != want {
        let _ = std::fs::remove_file(&part);
        return Err(format!("{} does not match its published size and SHA-256 ({} bytes, {}): not kept.", p.file, n, got));
    }
    std::fs::rename(&part, &dst).map_err(|e| {
        let _ = std::fs::remove_file(&part);
        format!("cannot move {}: {}", dst.display(), e)
    })?;
    Ok(true)
}
