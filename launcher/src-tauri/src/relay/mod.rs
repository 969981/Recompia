// SPDX-License-Identifier: GPL-3.0-or-later
//! LDN relay server ("pkp host"). Game clients emulate Nintendo local wireless (nn::ldn) and tunnel the game's UDP
//! through this server. Every player, the host player too, keeps one TCP connection to it. The server keeps rooms
//! (LDN networks) and routes datagrams between virtual IPs inside a room; it never parses game data.
//!
//! # Wire protocol v2 (reference for the C++ client)
//!
//! TCP, all integers little-endian.
//!
//! Frame = `u32 len` + `u8 type` + payload. `len` counts the bytes after the length field (type + payload), 1..=8192,
//! in both directions. A frame with len 0, len > 8192, an unknown type, a short payload, trailing bytes or a field
//! over its cap is a bad frame: the server answers ERROR 10 and closes the connection.
//! `str` = `u8 len` + UTF-8 bytes (invalid UTF-8 is a bad frame).
//!
//! Client -> server:
//!
//! | type | name      | payload |
//! |------|-----------|---------|
//! | 0x01 | HELLO     | u16 proto=2, str build stamp, str nickname (<= 32 B), str password |
//! | 0x02 | SCAN      | u64 local_comm_id (0 = any), u16 scene_id (0xffff = any), 16 B room key |
//! | 0x03 | CREATE    | u64 local_comm_id, u16 scene_id, s16 comm_version, u8 node_count_max (1..8), u8 security_mode, 16 B passphrase hash, 16 B room key, u16 adv_len + adv bytes (<= 384) |
//! | 0x04 | JOIN      | u32 room_id, s16 comm_version, 16 B passphrase hash, 16 B room key |
//! | 0x05 | LEAVE     | - |
//! | 0x06 | ADVERTISE | u16 len + bytes (<= 384), host only |
//! | 0x07 | REJECT    | u32 vip, host only |
//! | 0x08 | DATAGRAM  | u16 src_port, u32 dst_ip, u16 dst_port, u16 len + bytes (<= 4096) |
//! | 0x09 | PING      | u32 nonce |
//!
//! Server -> client:
//!
//! | type | name       | payload |
//! |------|------------|---------|
//! | 0x81 | WELCOME    | u32 client_id, str server version |
//! | 0x82 | ERROR      | u16 code, str message |
//! | 0x83 | ROOM_LIST  | u8 count, count x ROOM_INFO |
//! | 0x84 | JOINED     | u32 room_id, u32 your_vip, u8 your_node_id |
//! | 0x85 | ROOM_STATE | ROOM_INFO |
//! | 0x86 | DATAGRAM   | u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, u16 len + bytes |
//! | 0x87 | LEFT       | u16 reason (0 you left, 1 host closed, 2 rejected, 3 server shutdown) |
//! | 0x88 | PONG       | u32 nonce |
//!
//! `ROOM_INFO` = u32 room_id, u64 local_comm_id, u16 scene_id, s16 comm_version, u8 node_count_max, u8 security_mode,
//! u8 node_count, node_count x {u8 node_id, u32 vip, 6 B mac, str nickname}, u16 adv_len + adv bytes.
//!
//! ERROR codes: 1 bad proto, 2 build mismatch, 3 password, 4 room full, 5 room not found, 6 version mismatch,
//! 7 not host, 8 rejected, 9 rate limit, 10 bad frame, 11 wrong room code. An ERROR answering HELLO, ERROR 9 and ERROR 10 for a malformed
//! frame close the connection; the others leave it open. The message is plain English for the launcher/game to show; it never carries raw io errors.
//!
//! Protocol 2 = protocol 1 + the room key in SCAN, CREATE and JOIN (and ERROR 11). A protocol 1 client gets ERROR 1 with a
//! message saying to update the game.
//!
//! # Room codes
//!
//! A host may create its room with a room code: 16 symbols of Crockford base32 (`0-9 A-Z` without I L O U, 80 bits from the
//! OS random source), shown as `XXXX-XXXX-XXXX-XXXX`; on input case, `-` and spaces do not matter and O reads as 0, I and L as 1
//! (format from pokopia-native's coop_auth.c). The code never goes over the wire: clients send the room key, the first 16 bytes
//! of SHA-256("pokopia-relay room v1" + the 16 normalized symbols). A key of 16 zero bytes means "no code".
//! A room created with no code is an open room, listed to everyone (LAN play as before). A room created with a key is listed only
//! to SCANs carrying that key and joined only by JOINs carrying it. Wrong codes are counted and limited (see JOIN).
//!
//! # Semantics
//!
//! - HELLO must be the first frame. proto must be 2 (else ERROR 1); the build stamp must equal the server's when the
//!   server has one (else ERROR 2); the password must equal the server's when it has one (else ERROR 3).
//!   Success: WELCOME. Any other frame before HELLO, or a second HELLO: ERROR 10 + close.
//! - SCAN -> ROOM_LIST of the open rooms plus the rooms whose key equals the SCAN's, that are not full and match the filter. The list is cut so the frame stays
//!   <= 8192 B (a full ROOM_INFO is up to 757 B, 64 rooms would not fit).
//! - CREATE (caller in no room): new room R = lowest free 1..254, subnet 169.254.R.0/24; the caller is its host,
//!   vip 169.254.R.1, node 0. Reply JOINED then ROOM_STATE. No free room (64 max): ERROR 4.
//! - Room keys (SCAN, CREATE, JOIN): a connection keeps the first nonzero key it sends; a different nonzero key later counts as
//!   a wrong code (ERROR 11). Keys are compared in constant time.
//! - JOIN (caller in no room): room must exist (5); a room with a key: the JOIN's key must equal it (11, checked before anything
//!   else; no key counts as wrong). 5 wrong codes on one connection: ERROR 9 + close. 30 wrong codes from one IP address in
//!   10 minutes (the first keyed SCAN of a connection that finds nothing counts too): every request with a key from that address
//!   gets ERROR 9 + close until the window ends. Then: the caller not rejected from it (8), not full (4), comm_version
//!   equal (6), passphrase hash equal (3). Member gets vip 169.254.R.N with N = lowest free 2..=8, node id N-1.
//!   Reply JOINED, then ROOM_STATE to every member (the new one included).
//!   CREATE/JOIN while already in a room: ERROR 10 (connection stays open).
//! - mac of node N in room R = 02:00:a9:fe:R:N. vIPs are u32 in host order (169.254.1.2 = 0xa9fe0102).
//! - LEAVE: LEFT 0 to the caller. A guest leaving: ROOM_STATE to the rest. The host leaving closes the room: LEFT 1 to
//!   every guest. Disconnect and keepalive timeout leave the same way.
//! - ADVERTISE: host only (else ERROR 7); replaces the advertise data, ROOM_STATE to every member.
//! - REJECT: host only (else ERROR 7). The member with that vip gets LEFT 2 and is removed; the same connection cannot
//!   JOIN that room again (ERROR 8). ROOM_STATE to the rest. Unknown vip / the host's own vip: ignored.
//! - DATAGRAM: only from a room member (else dropped). The server sets src_ip to the sender's vip (no spoofing).
//!   dst_ip = 169.254.R.255: copied to every other member. dst_ip = a member's vip: sent to that member only.
//!   Anything else (unknown vip, the sender's own vip, another subnet): dropped silently. The sender gets none of
//!   its own datagrams back; the client loops broadcast/self traffic back locally if the game needs it.
//! - Keepalive: the client sends PING every 5 s (PONG echoes the nonce). A client that sends no frame for 15 s is
//!   dropped (as a disconnect).
//! - Caps: frame 8192 B, 8 nodes/room, 64 rooms, 32 connections (the 33rd gets ERROR 4 "server full" and is closed),
//!   2000 frames/s per client (ERROR 9 + close).
//! - Server stop: LEFT 3 to every room member, then all connections close.
//! - Slow clients: every client has a bounded send queue (256 frames) and a 5 s write timeout. A full queue or a
//!   timed-out write drops that client; nobody else waits on it.

mod codec;
mod proto;
pub mod room_code;
mod server;
#[cfg(test)]
mod tests;

pub use server::{Config, RelayServer, Status};

/// Default TCP port of the relay.
pub const DEFAULT_PORT: u16 = 39660;
