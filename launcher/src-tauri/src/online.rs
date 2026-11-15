// SPDX-License-Identifier: GPL-3.0-or-later
//! Online presence: while pkp.exe runs, and only while the player has it switched on, tells api.recompia.com that this player is online
//! (hello, then a heartbeat every half minute, then leave). What is sent is exactly what the setting's notice and recompia.com say: the
//! online name the player typed (none: the server shows "anonymous"), the build, the kind of device and the Pokemon-at-home count of the
//! save (home_count.rs). Never the save, the profile nickname or the account uid.
//!
//! The switch is `[pkp] online_presence` in pkp.ini (runtime/games/pkp/core/pkp_settings_table.h, not read by the runtime), so the
//! game's own Settings screen (its Online tab), the launcher's Home chip and Settings > Game all change the same key. It is read every
//! few seconds while the game runs: switched on in the game, presence starts; switched off, the watcher says leave at once.
//!
//! Identity: the token is SHA-256 of the profile uid and a random per-install salt (config.rs online_salt), so the server sees a stable
//! pseudonym per profile that cannot be turned back into the uid. "Reset my online ID" empties the salt; nothing else changes it. The
//! salt is made the first time presence actually starts, so a player who never switches it on never gets one.
//!
//! Best effort and silent, like presence.rs: a server that is down or slow only means no presence. Requests have short timeouts, the
//! game never waits on this, and `Watch::finish` gives the leave a few seconds, then lets go (the server marks a silent player offline
//! after 90 s anyway).

use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::path::Path;
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::time::{Duration, Instant};

use crate::ini::Ini;
use crate::settings::PKP_INI;

/// The presence API (jerbanpls/recompia-website api/). Public reads; writes need the token.
pub const SERVER: &str = "https://api.recompia.com";
/// pkp.ini `[pkp]` key of the switch.
pub const KEY: &str = "online_presence";
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const IO_TIMEOUT: Duration = Duration::from_secs(10);
/// The server asks for a heartbeat every `heartbeat_interval` seconds; an answer outside this range is not trusted.
const MIN_INTERVAL: Duration = Duration::from_secs(10);
const MAX_INTERVAL: Duration = Duration::from_secs(120);
const DEFAULT_INTERVAL: Duration = Duration::from_secs(30);
/// How often the switch in pkp.ini is read while the game runs.
const POLL: Duration = Duration::from_secs(5);
/// How long `finish` waits for the leave before it lets go.
const LEAVE_WAIT: Duration = Duration::from_secs(3);

/// True when `[pkp] online_presence` in this game folder's pkp.ini is on. Missing or anything else: off.
pub fn switched_on(game_dir: &Path) -> bool {
    let ini = std::fs::read(game_dir.join(PKP_INI)).ok().map(|b| Ini::parse(&String::from_utf8_lossy(&b)));
    let v = ini.and_then(|i| i.get("pkp", KEY)).unwrap_or_default();
    matches!(v.trim().to_ascii_lowercase().as_str(), "1" | "true" | "yes" | "on")
}

/// Who is online: everything a hello sends. Made once per game run, the first time presence starts.
#[derive(Clone, Debug, PartialEq)]
pub struct Player {
    pub token: String,
    /// The online name; None: anonymous.
    pub name: Option<String>,
    pub build: String,
    pub device: &'static str,
}

/// The online ID: SHA-256 of the profile uid and the install's salt, as hex.
pub fn token_of(uid: &str, salt: &str) -> String {
    let mut h = Sha256::new();
    h.update(b"recompia-online-v1\0");
    h.update(uid.as_bytes());
    h.update(b"\0");
    h.update(salt.as_bytes());
    h.finalize().iter().map(|b| format!("{:02x}", b)).collect()
}

/// The device word the server shows: the launcher's own platform.
pub fn device() -> &'static str {
    if cfg!(windows) { "windows" } else { "linux" }
}

impl Player {
    pub fn new(uid: &str, salt: &str, name: &str, build: &str) -> Player {
        let name = name.trim();
        Player { token: token_of(uid, salt), name: (!name.is_empty()).then(|| name.to_string()), build: build.to_string(), device: device() }
    }
}

/// The player for this run: the active profile's uid with the install's salt (made and saved now if there is none), the online name from
/// launcher.json and this launcher's version. None when no salt could be saved.
fn player_of(app: &tauri::AppHandle, game_dir: &Path) -> Option<Player> {
    let salt = crate::config::online_salt(app)?;
    let cfg = crate::config::load_config(app);
    let uid = crate::profiles::load(game_dir).active_profile().uid.clone();
    Some(Player::new(&uid, &salt, &cfg.online_name, &app.package_info().version.to_string()))
}

enum Failure {
    /// The server answered with this status (404/409 on a heartbeat: the session is gone, say hello again).
    Status(u16),
    /// No answer, or one that is not JSON.
    Network,
}

struct Client {
    base: String,
    agent: ureq::Agent,
}

impl Client {
    fn new(base: &str) -> Client {
        let agent = ureq::AgentBuilder::new()
            .timeout_connect(CONNECT_TIMEOUT)
            .timeout_read(IO_TIMEOUT)
            .timeout_write(IO_TIMEOUT)
            .user_agent(&format!("Recompia-Launcher/{}", env!("CARGO_PKG_VERSION")))
            .build();
        Client { base: base.trim_end_matches('/').to_string(), agent }
    }

    fn post(&self, token: &str, path: &str, body: &Value) -> Result<Value, Failure> {
        let res = self
            .agent
            .post(&format!("{}{}", self.base, path))
            .set("Authorization", &format!("Bearer {}", token))
            .set("Content-Type", "application/json")
            .send_string(&body.to_string());
        match res {
            Ok(r) => r.into_string().ok().and_then(|t| serde_json::from_str(&t).ok()).ok_or(Failure::Network),
            Err(ureq::Error::Status(code, _)) => Err(Failure::Status(code)),
            Err(_) => Err(Failure::Network),
        }
    }

    /// A new session: its id and the heartbeat interval the server asks for.
    fn hello(&self, p: &Player, count: Option<u32>) -> Option<(String, Duration)> {
        let body = json!({ "token": p.token, "nickname": p.name, "build": p.build, "device": p.device, "pokemon_in_home": count });
        let r = self.post(&p.token, "/v1/hello", &body).ok()?;
        let sid = r["session_id"].as_str()?.to_string();
        let every = r["heartbeat_interval"].as_u64().map_or(DEFAULT_INTERVAL, Duration::from_secs);
        Some((sid, every))
    }

    fn leave(&self, p: &Player, sid: &str) {
        let _ = self.post(&p.token, "/v1/leave", &json!({ "session_id": sid }));
    }
}

/// What one game run's watcher reads from outside: the switch, the player (made the first time it is needed) and the count.
struct Inputs<'a> {
    on: &'a dyn Fn() -> bool,
    player: &'a dyn Fn() -> Option<Player>,
    count: &'a dyn Fn() -> Option<u32>,
}

/// One game run's presence. Checks the switch every `poll`; `floor` is the shortest heartbeat interval accepted (MIN_INTERVAL; the
/// tests use less, and a shorter poll).
fn run(client: &Client, inp: &Inputs, stop: &Receiver<()>, poll: Duration, floor: Duration) {
    let mut player: Option<Player> = None;
    let mut session: Option<(String, Instant)> = None;   // id, when the next heartbeat is due
    let mut every = DEFAULT_INTERVAL;
    loop {
        if !(inp.on)() {
            if let (Some(p), Some((sid, _))) = (&player, session.take()) {
                client.leave(p, &sid);   // switched off while the game runs: gone from the list now, not after 90 s
            }
        } else {
            if player.is_none() {
                player = (inp.player)();
            }
            if let Some(p) = &player {
                match &session {
                    None => {
                        if let Some((sid, e)) = client.hello(p, (inp.count)()) {
                            every = e.clamp(floor, MAX_INTERVAL.max(floor));
                            session = Some((sid, Instant::now() + every));
                        }
                    }
                    Some((sid, due)) if Instant::now() >= *due => {
                        let body = json!({ "session_id": sid, "pokemon_in_home": (inp.count)() });
                        session = match client.post(&p.token, "/v1/heartbeat", &body) {
                            // the server let the session go (offline after silence, or restarted): a new hello on the next check
                            Err(Failure::Status(404 | 409)) => None,
                            _ => Some((sid.clone(), Instant::now() + every)),
                        };
                    }
                    Some(_) => {}
                }
            }
        }
        match stop.recv_timeout(poll) {
            Err(RecvTimeoutError::Timeout) => continue,
            _ => break,
        }
    }
    if let (Some(p), Some((sid, _))) = (&player, session) {
        client.leave(p, &sid);
    }
}

/// Presence for the length of one game run: started when pkp.exe starts, finished when it has ended. It sends nothing while the switch
/// is off, so starting it costs a pkp.ini read every few seconds and nothing else.
pub struct Watch {
    stop: Sender<()>,
    done: Receiver<()>,
}

impl Watch {
    pub fn start(app: &tauri::AppHandle, game_dir: &Path) -> Watch {
        let (app, dir) = (app.clone(), game_dir.to_path_buf());
        Watch::spawn(SERVER, move |client, stop| {
            let inp = Inputs {
                on: &|| switched_on(&dir),
                player: &|| player_of(&app, &dir),
                count: &|| crate::home_count::home_count(&dir),
            };
            run(client, &inp, stop, POLL, MIN_INTERVAL);
        })
    }

    fn spawn(base: &str, body: impl FnOnce(&Client, &Receiver<()>) + Send + 'static) -> Watch {
        let (stop_tx, stop_rx) = channel();
        let (done_tx, done_rx) = channel();
        let client = Client::new(base);
        std::thread::spawn(move || {
            body(&client, &stop_rx);
            let _ = done_tx.send(());
        });
        Watch { stop: stop_tx, done: done_rx }
    }

    /// The game has ended: says leave if presence is on. A server that does not answer is let go after a few seconds.
    pub fn finish(self) {
        let _ = self.stop.send(());
        let _ = self.done.recv_timeout(LEAVE_WAIT);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};

    /// What a stub server was sent: (path, Authorization header, JSON body).
    type Seen = Arc<Mutex<Vec<(String, String, Value)>>>;

    /// A presence API on a local port. `answer(path, n)` gives the status and JSON for the n-th request (0-based).
    fn stub(answer: fn(&str, usize) -> (u16, Value)) -> (String, Seen) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", l.local_addr().unwrap());
        let seen: Seen = Arc::default();
        let log = seen.clone();
        std::thread::spawn(move || {
            for (n, s) in l.incoming().flatten().enumerate() {
                let mut r = BufReader::new(s.try_clone().unwrap());
                let mut line = String::new();
                r.read_line(&mut line).unwrap();
                let path = line.split_whitespace().nth(1).unwrap_or_default().to_string();
                let (mut len, mut auth) = (0, String::new());
                loop {
                    let mut h = String::new();
                    r.read_line(&mut h).unwrap();
                    if h.trim().is_empty() {
                        break;
                    }
                    let (k, v) = h.split_once(':').unwrap();
                    match k.to_ascii_lowercase().as_str() {
                        "content-length" => len = v.trim().parse().unwrap(),
                        "authorization" => auth = v.trim().to_string(),
                        _ => {}
                    }
                }
                let mut body = vec![0; len];
                r.read_exact(&mut body).unwrap();
                log.lock().unwrap().push((path.clone(), auth, serde_json::from_slice(&body).unwrap()));
                let (code, json) = answer(&path, n);
                let text = json.to_string();
                let mut s = s;
                let _ = write!(s, "HTTP/1.1 {} X\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", code, text.len(), text);
            }
        });
        (base, seen)
    }

    fn ok(path: &str, _: usize) -> (u16, Value) {
        match path {
            "/v1/hello" => (200, json!({ "session_id": "s1", "server_time": 1, "heartbeat_interval": 0 })),
            _ => (200, json!({ "ok": true })),
        }
    }

    fn player() -> Player {
        Player::new("0123456789abcdef0123456789abcdef", "salt", "  Misty ", "0.10.4")
    }

    fn paths(seen: &Seen) -> Vec<String> {
        seen.lock().unwrap().iter().map(|s| s.0.clone()).collect()
    }

    fn wait_for(seen: &Seen, cond: impl Fn(&[String]) -> bool) {
        let t = Instant::now();
        while !cond(&paths(seen)) {
            assert!(t.elapsed() < Duration::from_secs(10), "timed out: {:?}", paths(seen));
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    /// Runs the watcher against `base` with a switch the test flips; returns the stop sender and the thread.
    fn watch(base: &str, on: Arc<AtomicBool>, made: Arc<Mutex<u32>>) -> (Sender<()>, std::thread::JoinHandle<()>) {
        let client = Client::new(base);
        let (tx, rx) = channel();
        let h = std::thread::spawn(move || {
            let inp = Inputs {
                on: &|| on.load(Ordering::SeqCst),
                player: &|| {
                    *made.lock().unwrap() += 1;
                    Some(player())
                },
                count: &|| Some(64),
            };
            run(&client, &inp, &rx, Duration::from_millis(10), Duration::from_millis(20));
        });
        (tx, h)
    }

    #[test]
    fn the_token_is_a_stable_hash_that_hides_the_uid_and_changes_with_the_salt() {
        let a = token_of("0123456789abcdef0123456789abcdef", "salt");
        assert_eq!(a, token_of("0123456789abcdef0123456789abcdef", "salt"));
        assert_eq!(a.len(), 64);
        assert!(!a.contains("0123456789abcdef"));
        assert_ne!(a, token_of("0123456789abcdef0123456789abcdef", "other salt"));
        assert_ne!(a, token_of("fedcba9876543210fedcba9876543210", "salt"));
    }

    #[test]
    fn an_empty_online_name_is_anonymous() {
        assert_eq!(Player::new("u", "s", "   ", "b").name, None);
        assert_eq!(player().name.as_deref(), Some("Misty"));
    }

    #[test]
    fn the_switch_is_read_from_pkp_ini_and_anything_else_is_off() {
        let d = std::env::temp_dir().join(format!("pkp-online-switch-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        assert!(!switched_on(&d), "no pkp.ini");
        for (text, on) in [("[pkp]\nonline_presence = 1\n", true), ("[pkp]\nonline_presence = true\n", true), ("[pkp]\nonline_presence = 0\n", false),
                           ("[pkp]\nonline_presence = maybe\n", false), ("[other]\nonline_presence = 1\n", false), ("[pkp]\n", false)] {
            std::fs::write(d.join(PKP_INI), text).unwrap();
            assert_eq!(switched_on(&d), on, "{:?}", text);
        }
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn hello_heartbeat_leave_send_only_what_the_notice_says() {
        let (base, seen) = stub(ok);
        let (tx, h) = watch(&base, Arc::new(AtomicBool::new(true)), Arc::default());
        wait_for(&seen, |p| p.len() >= 2);
        tx.send(()).unwrap();
        h.join().unwrap();
        let p = player();
        let seen = seen.lock().unwrap();
        let paths: Vec<&str> = seen.iter().map(|s| s.0.as_str()).collect();
        assert_eq!(paths.first(), Some(&"/v1/hello"));
        assert!(paths[1..paths.len() - 1].iter().all(|p| *p == "/v1/heartbeat"), "{:?}", paths);
        assert_eq!(paths.last(), Some(&"/v1/leave"));
        assert!(seen.iter().all(|s| s.1 == format!("Bearer {}", p.token)));
        let hello = &seen[0].2;
        let mut keys: Vec<&str> = hello.as_object().unwrap().keys().map(|k| k.as_str()).collect();
        keys.sort();
        assert_eq!(keys, ["build", "device", "nickname", "pokemon_in_home", "token"]);
        assert_eq!(hello["nickname"], "Misty");
        assert_eq!(hello["pokemon_in_home"], 64);
        assert_eq!(hello["device"], device());
        assert_eq!(seen[1].2, json!({ "session_id": "s1", "pokemon_in_home": 64 }));
        assert_eq!(seen.last().unwrap().2, json!({ "session_id": "s1" }));
    }

    #[test]
    fn off_sends_nothing_and_makes_no_online_id() {
        let (base, seen) = stub(ok);
        let made = Arc::new(Mutex::new(0));
        let (tx, h) = watch(&base, Arc::new(AtomicBool::new(false)), made.clone());
        std::thread::sleep(Duration::from_millis(100));
        tx.send(()).unwrap();
        h.join().unwrap();
        assert!(paths(&seen).is_empty());
        assert_eq!(*made.lock().unwrap(), 0, "no salt is made for a player who never switched it on");
    }

    #[test]
    fn switching_off_in_the_game_leaves_and_on_again_says_hello() {
        let (base, seen) = stub(ok);
        let on = Arc::new(AtomicBool::new(true));
        let made = Arc::new(Mutex::new(0));
        let (tx, h) = watch(&base, on.clone(), made.clone());
        wait_for(&seen, |p| p.first().map(|s| s.as_str()) == Some("/v1/hello"));
        on.store(false, Ordering::SeqCst);
        wait_for(&seen, |p| p.last().map(|s| s.as_str()) == Some("/v1/leave"));
        let n = paths(&seen).len();
        std::thread::sleep(Duration::from_millis(80));
        assert_eq!(paths(&seen).len(), n, "nothing is sent while it is off");
        on.store(true, Ordering::SeqCst);
        wait_for(&seen, |p| p.len() > n && p[n] == "/v1/hello");
        tx.send(()).unwrap();
        h.join().unwrap();
        assert_eq!(paths(&seen).last().map(|s| s.as_str()), Some("/v1/leave"));
        assert_eq!(*made.lock().unwrap(), 1, "the player is made once per run");
    }

    #[test]
    fn a_lost_session_says_hello_again() {
        fn gone_once(path: &str, n: usize) -> (u16, Value) {
            if path == "/v1/heartbeat" && n == 1 { (409, json!({ "error": "offline" })) } else { ok(path, n) }
        }
        let (base, seen) = stub(gone_once);
        let (tx, h) = watch(&base, Arc::new(AtomicBool::new(true)), Arc::default());
        wait_for(&seen, |p| p.len() >= 3);
        tx.send(()).unwrap();
        h.join().unwrap();
        let p = paths(&seen);
        assert_eq!(&p[..3], ["/v1/hello", "/v1/heartbeat", "/v1/hello"]);
    }

    #[test]
    fn no_server_is_silent_and_finish_returns_quickly() {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", l.local_addr().unwrap());
        drop(l);   // nothing listens there now
        let t = Instant::now();
        let w = Watch::spawn(&base, |client, stop| {
            let inp = Inputs { on: &|| true, player: &|| Some(player()), count: &|| None };
            run(client, &inp, stop, Duration::from_millis(10), Duration::from_millis(20));
        });
        std::thread::sleep(Duration::from_millis(50));
        w.finish();
        assert!(t.elapsed() < LEAVE_WAIT + Duration::from_secs(1));
    }
}
