// SPDX-License-Identifier: GPL-3.0-or-later
//! Discord Rich Presence: "Playing Recompia" with the time since the game started, while pkp.exe runs (when the player switched it on).
//!
//! Talks to the Discord desktop app over its local IPC (a named pipe on Windows, a unix socket on Linux), the protocol the official RPC
//! libraries use: a frame is a u32 opcode, a u32 length (both little endian) and a JSON text. Nothing leaves this PC from here: Discord
//! itself shows the activity. Only the game's running time is shared, never a name, a save or a setting.
//!
//! Best effort and silent: no Discord running, a pipe that fails or an answer that never comes only means no presence. Discord is
//! retried while the game runs (it may start later) and the activity is sent again every half minute, so a Discord restart is noticed.
//! The launcher never waits on Discord for long: `Watch::finish` gives it a few seconds to clear the activity, then lets go (Discord
//! drops an activity when its connection closes anyway).

use serde_json::{json, Value};
use std::io::{self, Read, Write};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// The Recompia application in the Discord Developer Portal. Public: it names the app, it is not a secret.
const APPLICATION_ID: &str = "1569938139974475776";
/// Art asset key of the large image (Developer Portal > Rich Presence > Art Assets). A key Discord does not know shows no image.
const LARGE_IMAGE: &str = "logo";
const LARGE_TEXT: &str = "Recompia";

const OP_HANDSHAKE: u32 = 0;
const OP_FRAME: u32 = 1;
/// A reply is never this big; a length past it means the stream is not Discord's.
const MAX_FRAME: u32 = 64 * 1024;
/// Discord was not there: look again this often while the game runs.
const RETRY: Duration = Duration::from_secs(15);
/// The activity is sent again this often; a failed send is how a closed Discord is noticed.
const REFRESH: Duration = Duration::from_secs(30);
/// How long `finish` waits for the activity to be cleared before it lets go.
const CLEAR_WAIT: Duration = Duration::from_secs(3);

trait Link: Read + Write {}
impl<T: Read + Write> Link for T {}

#[cfg(windows)]
fn connect() -> Option<Box<dyn Link>> {
    (0..10).find_map(|n| {
        let pipe = std::fs::OpenOptions::new().read(true).write(true).open(format!(r"\\?\pipe\discord-ipc-{}", n)).ok()?;
        Some(Box::new(pipe) as Box<dyn Link>)
    })
}

/// Discord puts its socket in the runtime folder; the Flatpak and Snap builds put it in their own sub-folder.
#[cfg(unix)]
fn connect() -> Option<Box<dyn Link>> {
    use std::os::unix::net::UnixStream;
    let bases: Vec<String> = ["XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"].iter().filter_map(|k| std::env::var(k).ok()).chain(["/tmp".to_string()]).collect();
    let subs = ["", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/", "snap.discord/", "snap.discord-canary/"];
    bases.iter().flat_map(|b| subs.iter().map(move |s| (b, s))).find_map(|(b, s)| {
        (0..10).find_map(|n| {
            let sock = UnixStream::connect(format!("{}/{}discord-ipc-{}", b.trim_end_matches('/'), s, n)).ok()?;
            sock.set_read_timeout(Some(Duration::from_secs(5))).ok()?;
            sock.set_write_timeout(Some(Duration::from_secs(5))).ok()?;
            Some(Box::new(sock) as Box<dyn Link>)
        })
    })
}

fn write_frame(link: &mut dyn Link, op: u32, body: &Value) -> io::Result<()> {
    let text = body.to_string();
    let mut buf = Vec::with_capacity(8 + text.len());
    buf.extend_from_slice(&op.to_le_bytes());
    buf.extend_from_slice(&(text.len() as u32).to_le_bytes());
    buf.extend_from_slice(text.as_bytes());
    link.write_all(&buf)?;
    link.flush()
}

/// Reads one frame and drops it: the answers (READY, the activity echo) are not needed, but the stream must be kept empty.
fn skip_frame(link: &mut dyn Link) -> io::Result<()> {
    let mut head = [0u8; 8];
    link.read_exact(&mut head)?;
    let len = u32::from_le_bytes([head[4], head[5], head[6], head[7]]);
    if len > MAX_FRAME {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "not a Discord frame"));
    }
    io::copy(&mut link.take(len as u64), &mut io::sink())?;
    Ok(())
}

/// SET_ACTIVITY; `started` none clears the activity.
fn set_activity(link: &mut dyn Link, nonce: &mut u64, started: Option<u64>) -> io::Result<()> {
    *nonce += 1;
    let activity = started.map(|t| {
        json!({
            "timestamps": { "start": t },
            "assets": { "large_image": LARGE_IMAGE, "large_text": LARGE_TEXT },
        })
    });
    let body = json!({ "cmd": "SET_ACTIVITY", "nonce": nonce.to_string(), "args": { "pid": std::process::id(), "activity": activity } });
    write_frame(link, OP_FRAME, &body)?;
    skip_frame(link)
}

/// One connection, from handshake to the cleared activity. Ok: asked to stop (the activity was cleared); Err: the link failed.
fn session(link: &mut dyn Link, stop: &Receiver<()>, started: u64) -> io::Result<()> {
    write_frame(link, OP_HANDSHAKE, &json!({ "v": 1, "client_id": APPLICATION_ID }))?;
    skip_frame(link)?;
    let mut nonce = 0;
    loop {
        set_activity(link, &mut nonce, Some(started))?;
        match stop.recv_timeout(REFRESH) {
            Err(RecvTimeoutError::Timeout) => continue,
            _ => return set_activity(link, &mut nonce, None),
        }
    }
}

fn run(stop: &Receiver<()>, started: u64) {
    loop {
        if let Some(mut link) = connect() {
            if session(&mut *link, stop, started).is_ok() {
                return;
            }
        }
        if !matches!(stop.recv_timeout(RETRY), Err(RecvTimeoutError::Timeout)) {
            return;
        }
    }
}

/// Presence for the length of one game run: started when pkp.exe starts, finished when it has ended. Does nothing when switched off.
pub struct Watch {
    live: Option<(Sender<()>, Receiver<()>)>,
}

impl Watch {
    pub fn start(enabled: bool) -> Watch {
        if !enabled {
            return Watch { live: None };
        }
        let (stop_tx, stop_rx) = channel();
        let (done_tx, done_rx) = channel();
        let started = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs());
        std::thread::spawn(move || {
            run(&stop_rx, started);
            let _ = done_tx.send(());
        });
        Watch { live: Some((stop_tx, done_rx)) }
    }

    /// The game has ended: clears the activity. A Discord that does not answer is let go after a few seconds.
    pub fn finish(self) {
        if let Some((stop, done)) = self.live {
            let _ = stop.send(());
            let _ = done.recv_timeout(CLEAR_WAIT);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    /// A Discord that only answers every frame with an empty JSON object, and keeps what it was sent.
    struct Fake {
        replies: Cursor<Vec<u8>>,
        sent: Vec<u8>,
    }
    impl Read for Fake {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            self.replies.read(buf)
        }
    }
    impl Write for Fake {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.sent.extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    fn fake(answers: usize) -> Fake {
        let one = [&OP_FRAME.to_le_bytes()[..], &2u32.to_le_bytes()[..], b"{}"].concat();
        Fake { replies: Cursor::new(one.repeat(answers)), sent: vec![] }
    }

    /// The frames a Fake was sent: (opcode, JSON).
    fn frames(sent: &[u8]) -> Vec<(u32, Value)> {
        let mut out = vec![];
        let mut at = 0;
        while at < sent.len() {
            let op = u32::from_le_bytes(sent[at..at + 4].try_into().unwrap());
            let len = u32::from_le_bytes(sent[at + 4..at + 8].try_into().unwrap()) as usize;
            out.push((op, serde_json::from_slice(&sent[at + 8..at + 8 + len]).unwrap()));
            at += 8 + len;
        }
        out
    }

    #[test]
    fn handshake_then_activity_then_clear() {
        let mut link = fake(3);
        let (tx, rx) = channel();
        tx.send(()).unwrap();   // already asked to stop: one activity, then the clear
        session(&mut link, &rx, 1_700_000_000).unwrap();
        let sent = frames(&link.sent);
        assert_eq!(sent.len(), 3);
        assert_eq!(sent[0], (OP_HANDSHAKE, json!({ "v": 1, "client_id": APPLICATION_ID })));
        assert_eq!(sent[1].0, OP_FRAME);
        assert_eq!(sent[1].1["cmd"], "SET_ACTIVITY");
        assert_eq!(sent[1].1["args"]["activity"]["timestamps"]["start"], 1_700_000_000u64);
        assert_eq!(sent[1].1["args"]["activity"]["assets"]["large_image"], LARGE_IMAGE);
        assert!(sent[2].1["args"]["activity"].is_null());
    }

    #[test]
    fn a_stream_that_is_not_discord_is_refused() {
        let mut link = Fake { replies: Cursor::new([&OP_FRAME.to_le_bytes()[..], &u32::MAX.to_le_bytes()[..]].concat()), sent: vec![] };
        assert!(skip_frame(&mut link).is_err());
    }

    #[test]
    fn a_closed_link_is_an_error_not_a_hang() {
        let mut link = fake(0);
        let (_tx, rx) = channel();
        assert!(session(&mut link, &rx, 1).is_err());
    }

    #[test]
    fn off_means_no_thread_and_finish_returns_at_once() {
        Watch::start(false).finish();
    }
}
