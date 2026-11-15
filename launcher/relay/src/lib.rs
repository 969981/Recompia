// SPDX-License-Identifier: GPL-3.0-or-later
//! The relay crate: the launcher's "Run a relay here" and `--host`, and the standalone `pokopia-relay` server binary (src/main.rs).
//!
//! Based on pokopia-native (MIT) windows/tools/coop-relay/coop_relay.h @4a72d23: the Link Play relay ("pkp host"), a Rust port of
//! pokopia-native's coop-relay, the meeting point for internet play when the host cannot forward a port. Game builds with `[net] relay` set
//! (ours and pokopia-native's) all dial out to it.
//!
//! It keeps rooms keyed by a 16-byte room id the players derive from their room code (coop_auth.c croom_keys): it never sees a private code or any
//! key (a listed room's code is public, see "Room list"). A host keeps a control link open for its room; a visitor asks for the room, the relay tells the host, the host dials one more link for
//! that visitor, and the relay pairs the two and copies bytes between them without reading them. Everything inside is the players' own
//! authenticated protocol (coop protocol 3), so the relay cannot forge or change game messages; it CAN read them (nothing is encrypted).
//!
//! # Protocol (PKR1, TCP, port 47610, little-endian; the same values as runtime/third_party/pokopia-coop/coop_int.h RLY_*)
//!
//! A link's first 32 bytes: u32 magic "PKR1", u8 kind (1 host, 2 join, 3 attach), u8 version 1, 2 reserved, room id[16], u32 visitor id,
//! u32 cookie. The relay answers in 16-byte messages: u32 magic, u8 type (1 ok, 2 visitor, 3 gone, 4 error, 5 pong), u8 error code
//! (1 no room, 2 taken, 3 full, 4 limit, 5 timeout, 6 bad, 7 expired), 2 reserved, u32 arg (a visitor id), u32 cookie. A host's control link
//! may send pings (type 6).
//!
//! Limits (engine::Config, coop_relay_defaults): 64 rooms, 7 visitors a room, 256 links, 24 links and 10 new links a second per address,
//! 60 failed lookups a minute per address, hello within 5 s, a visitor waits 10 s for its host, a host's control link silent 30 s closes its
//! room, a room lives 12 h and carries 4 GiB, a link sends 256 KiB/s (bursts twice that).
//!
//! # Relay UDP (pokopia-recomp extension, opt-in, coop_int.h "pokopia-recomp extension: relay UDP")
//!
//! pokopia-native sends game datagrams over UDP on direct links but over the TCP link through a relay (one lost segment stalls every datagram
//! behind it). This relay can also carry them over UDP, only between two links that both ask for it, so their builds and their relay see
//! exactly the PKR1 above:
//!
//! - Hello byte 6 = 0xC5: this link wants relay UDP (their clients send 0, their relay ignores the byte).
//! - At pairing, when the visitor's join and the host's attach both asked (and the UDP socket is open): message type 0xC5 to each link, arg =
//!   the low and cookie = the high 32 bits of that link's random u64 UDP token; the host's link gets it before any byte of the visitor's,
//!   the visitor before its OK. Otherwise nothing extra is sent and both stay on TCP.
//! - UDP on the relay's port, packets of u32 magic "PRU1", u8 type, u8 flags, 2 reserved, u64 token, payload:
//!   1 BIND (client to relay, no payload): this token's endpoint is the sender. Answered with 2 BOUND (as long as the bind, flags bit 0: the
//!   other link of the pair has bound too). 3 DATA: payload for the other link of the pair, sent on to its endpoint with its own token.
//! - A packet counts only when its token is a paired link's and it comes from that link's TCP address; data shares the link's byte rate and
//!   the room's byte budget. The payload is the players' own tagged datagram (PCU3: token, sequence number, HMAC tag), checked end to end.
//! - The clients also agree end to end, over their tagged link (frame type 0xC5), before either sends game datagrams this way.
//!
//! # Room list (pokopia-recomp extension, opt-in, coop_int.h "pokopia-recomp extension: room list")
//!
//! Internet play through a relay with no room code set: the host lists its room and anyone can find it with the game's own search (Visit a
//! Friend); the game's own Link Code keeps a room private (the game checks it). A listed room's code is public: it only keeps the relay's room
//! ids apart and tags the players' messages against changes on the way. Their relay and their clients never see any of this:
//!
//! - The OK to a host's hello has arg 1 (X_CAP_LIST): this relay lists rooms (their relay sends 0, their clients ignore the arg).
//! - Then the host may send, on its control link, a 16-byte message of type 0xC6 (arg = a record's length, 0x94..0x4AC) and the record:
//!   u8 version 1, u8 flags 0, u16 NetworkInfo length (0x68..0x480), the room code (16 bytes, normalized Crockford base32), main's build id
//!   (its first 16 hex digits, ASCII; zeros: unknown), u64 mods fingerprint, the first bytes of the host's NetworkInfo (the rest are zeros).
//!   The relay checks the code against the room's id (the players' own derivation, hkdf.rs) and the player counts, and lists the room until it
//!   closes. A new record replaces the last one (at most one every 250 ms counts).
//! - Hello kind 4 (LIST), room id field = a build id (16 ASCII), visitor id + cookie fields = a mods fingerprint (u64); byte 6 = filter (bit 0
//!   by build, bit 1 by mods), byte 7 = entries wanted (0: 32, at most 32). Answer: type 0xC7 (arg = entries following | rooms open << 16,
//!   cookie = players in the listed rooms), then per room type 0xC8 (arg = length) and its record, rooms with a free place first, then the
//!   newest. Then the relay closes the link. 120 lists a minute per address (bursts as much). A relay without the list refuses kind 4 ("bad").
//! - A visitor joins a listed room as any other: the room id and the keys come from the listed code.

pub mod cli;
mod engine;
mod hkdf;
mod poll;
pub mod random;
mod server;

pub use engine::Config;
pub use server::{RelayServer, Status};

/// Default port of the relay (TCP, and UDP for relay UDP): pokopia-native's COOP_RELAY_PORT.
pub const DEFAULT_PORT: u16 = 47610;
