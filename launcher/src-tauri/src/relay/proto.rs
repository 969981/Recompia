// SPDX-License-Identifier: GPL-3.0-or-later
//! Protocol v2 messages (layouts in mod.rs). Both directions encode and decode: the server uses client decode + server
//! encode; the tests (and the C++ client, by mirror) use the other half.

use super::codec::{BadFrame, In, Out, MAX_FRAME};

pub const PROTO: u16 = 2;
/// A room key of all zeros: no room code (an open room, listed to everyone).
pub const NO_KEY: [u8; 16] = [0; 16];
pub const MAX_NICK: usize = 32;
pub const MAX_ADV: usize = 384;
pub const MAX_DATA: usize = 4096;
pub const MAX_NODES: u8 = 8;

pub mod err {
    pub const BAD_PROTO: u16 = 1;
    pub const BUILD: u16 = 2;
    pub const PASSWORD: u16 = 3;
    pub const ROOM_FULL: u16 = 4;
    pub const NO_ROOM: u16 = 5;
    pub const VERSION: u16 = 6;
    pub const NOT_HOST: u16 = 7;
    pub const REJECTED: u16 = 8;
    pub const RATE: u16 = 9;
    pub const BAD_FRAME: u16 = 10;
    pub const ROOM_CODE: u16 = 11;
}

pub mod left {
    pub const YOU_LEFT: u16 = 0;
    pub const HOST_CLOSED: u16 = 1;
    pub const REJECTED: u16 = 2;
    pub const SHUTDOWN: u16 = 3;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateReq {
    pub local_comm_id: u64,
    pub scene_id: u16,
    pub comm_version: i16,
    pub node_count_max: u8,
    pub security_mode: u8,
    pub pass_hash: [u8; 16],
    /// NO_KEY: an open room. Else the room is listed to, and joinable by, clients presenting this key only.
    pub room_key: [u8; 16],
    pub adv: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClientMsg {
    Hello { proto: u16, build: String, nickname: String, password: String },
    Scan { local_comm_id: u64, scene_id: u16, room_key: [u8; 16] },
    Create(CreateReq),
    Join { room_id: u32, comm_version: i16, pass_hash: [u8; 16], room_key: [u8; 16] },
    Leave,
    Advertise(Vec<u8>),
    Reject { vip: u32 },
    Datagram { src_port: u16, dst_ip: u32, dst_port: u16, data: Vec<u8> },
    Ping(u32),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NodeInfo {
    pub node_id: u8,
    pub vip: u32,
    pub mac: [u8; 6],
    pub nickname: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RoomInfo {
    pub room_id: u32,
    pub local_comm_id: u64,
    pub scene_id: u16,
    pub comm_version: i16,
    pub node_count_max: u8,
    pub security_mode: u8,
    pub nodes: Vec<NodeInfo>,
    pub adv: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ServerMsg {
    Welcome { client_id: u32, version: String },
    Error { code: u16, message: String },
    RoomList(Vec<RoomInfo>),
    Joined { room_id: u32, vip: u32, node_id: u8 },
    RoomState(RoomInfo),
    Datagram { src_ip: u32, src_port: u16, dst_ip: u32, dst_port: u16, data: Vec<u8> },
    Left(u16),
    Pong(u32),
}

impl ClientMsg {
    pub fn decode(ty: u8, payload: &[u8]) -> Result<Self, BadFrame> {
        let mut r = In::new(payload);
        let m = match ty {
            0x01 => {
                let proto = r.u16()?;
                let build = r.str()?;
                let nickname = r.str()?;
                if nickname.len() > MAX_NICK {
                    return Err(BadFrame);
                }
                ClientMsg::Hello { proto, build, nickname, password: r.str()? }
            }
            0x02 => ClientMsg::Scan { local_comm_id: r.u64()?, scene_id: r.u16()?, room_key: r.arr()? },
            0x03 => ClientMsg::Create(CreateReq {
                local_comm_id: r.u64()?,
                scene_id: r.u16()?,
                comm_version: r.i16()?,
                node_count_max: match r.u8()? {
                    n @ 1..=MAX_NODES => n,
                    _ => return Err(BadFrame),
                },
                security_mode: r.u8()?,
                pass_hash: r.arr()?,
                room_key: r.arr()?,
                adv: r.blob(MAX_ADV)?,
            }),
            0x04 => ClientMsg::Join { room_id: r.u32()?, comm_version: r.i16()?, pass_hash: r.arr()?, room_key: r.arr()? },
            0x05 => ClientMsg::Leave,
            0x06 => ClientMsg::Advertise(r.blob(MAX_ADV)?),
            0x07 => ClientMsg::Reject { vip: r.u32()? },
            0x08 => ClientMsg::Datagram { src_port: r.u16()?, dst_ip: r.u32()?, dst_port: r.u16()?, data: r.blob(MAX_DATA)? },
            0x09 => ClientMsg::Ping(r.u32()?),
            _ => return Err(BadFrame),
        };
        r.end()?;
        Ok(m)
    }

    #[allow(dead_code)] // client side: tests, and the reference for the C++ client
    pub fn encode(&self) -> Vec<u8> {
        match self {
            ClientMsg::Hello { proto, build, nickname, password } => {
                let mut o = Out::new(0x01);
                o.u16(*proto).str(build).str(nickname).str(password);
                o.finish()
            }
            ClientMsg::Scan { local_comm_id, scene_id, room_key } => {
                let mut o = Out::new(0x02);
                o.u64(*local_comm_id).u16(*scene_id).raw(room_key);
                o.finish()
            }
            ClientMsg::Create(c) => {
                let mut o = Out::new(0x03);
                o.u64(c.local_comm_id).u16(c.scene_id).i16(c.comm_version).u8(c.node_count_max).u8(c.security_mode);
                o.raw(&c.pass_hash).raw(&c.room_key).blob(&c.adv);
                o.finish()
            }
            ClientMsg::Join { room_id, comm_version, pass_hash, room_key } => {
                let mut o = Out::new(0x04);
                o.u32(*room_id).i16(*comm_version).raw(pass_hash).raw(room_key);
                o.finish()
            }
            ClientMsg::Leave => Out::new(0x05).finish(),
            ClientMsg::Advertise(adv) => {
                let mut o = Out::new(0x06);
                o.blob(adv);
                o.finish()
            }
            ClientMsg::Reject { vip } => {
                let mut o = Out::new(0x07);
                o.u32(*vip);
                o.finish()
            }
            ClientMsg::Datagram { src_port, dst_ip, dst_port, data } => {
                let mut o = Out::new(0x08);
                o.u16(*src_port).u32(*dst_ip).u16(*dst_port).blob(data);
                o.finish()
            }
            ClientMsg::Ping(n) => {
                let mut o = Out::new(0x09);
                o.u32(*n);
                o.finish()
            }
        }
    }
}

fn put_room(o: &mut Out, r: &RoomInfo) {
    o.u32(r.room_id).u64(r.local_comm_id).u16(r.scene_id).i16(r.comm_version).u8(r.node_count_max).u8(r.security_mode);
    o.u8(r.nodes.len() as u8);
    for n in &r.nodes {
        o.u8(n.node_id).u32(n.vip).raw(&n.mac).str(&n.nickname);
    }
    o.blob(&r.adv);
}

#[allow(dead_code)] // client side
fn get_room(r: &mut In) -> Result<RoomInfo, BadFrame> {
    let (room_id, local_comm_id, scene_id, comm_version, node_count_max, security_mode) =
        (r.u32()?, r.u64()?, r.u16()?, r.i16()?, r.u8()?, r.u8()?);
    let count = r.u8()?;
    let mut nodes = Vec::with_capacity(count as usize);
    for _ in 0..count {
        nodes.push(NodeInfo { node_id: r.u8()?, vip: r.u32()?, mac: r.arr()?, nickname: r.str()? });
    }
    Ok(RoomInfo { room_id, local_comm_id, scene_id, comm_version, node_count_max, security_mode, nodes, adv: r.blob(MAX_ADV)? })
}

impl ServerMsg {
    pub fn encode(&self) -> Vec<u8> {
        match self {
            ServerMsg::Welcome { client_id, version } => {
                let mut o = Out::new(0x81);
                o.u32(*client_id).str(version);
                o.finish()
            }
            ServerMsg::Error { code, message } => {
                let mut o = Out::new(0x82);
                o.u16(*code).str(message);
                o.finish()
            }
            ServerMsg::RoomList(rooms) => {
                // cut the list so the frame stays within MAX_FRAME (the u8 count is patched after)
                let mut o = Out::new(0x83);
                o.u8(0);
                let mut n = 0u8;
                for r in rooms.iter().take(255) {
                    let mut one = Out::new(0);
                    put_room(&mut one, r);
                    let body = &one.finish()[5..];
                    if o.len() - 4 + body.len() > MAX_FRAME {
                        break;
                    }
                    o.raw(body);
                    n += 1;
                }
                let mut v = o.finish();
                v[5] = n;
                v
            }
            ServerMsg::Joined { room_id, vip, node_id } => {
                let mut o = Out::new(0x84);
                o.u32(*room_id).u32(*vip).u8(*node_id);
                o.finish()
            }
            ServerMsg::RoomState(r) => {
                let mut o = Out::new(0x85);
                put_room(&mut o, r);
                o.finish()
            }
            ServerMsg::Datagram { src_ip, src_port, dst_ip, dst_port, data } => {
                let mut o = Out::new(0x86);
                o.u32(*src_ip).u16(*src_port).u32(*dst_ip).u16(*dst_port).blob(data);
                o.finish()
            }
            ServerMsg::Left(reason) => {
                let mut o = Out::new(0x87);
                o.u16(*reason);
                o.finish()
            }
            ServerMsg::Pong(n) => {
                let mut o = Out::new(0x88);
                o.u32(*n);
                o.finish()
            }
        }
    }

    #[allow(dead_code)] // client side
    pub fn decode(ty: u8, payload: &[u8]) -> Result<Self, BadFrame> {
        let mut r = In::new(payload);
        let m = match ty {
            0x81 => ServerMsg::Welcome { client_id: r.u32()?, version: r.str()? },
            0x82 => ServerMsg::Error { code: r.u16()?, message: r.str()? },
            0x83 => {
                let count = r.u8()?;
                let mut rooms = Vec::with_capacity(count as usize);
                for _ in 0..count {
                    rooms.push(get_room(&mut r)?);
                }
                ServerMsg::RoomList(rooms)
            }
            0x84 => ServerMsg::Joined { room_id: r.u32()?, vip: r.u32()?, node_id: r.u8()? },
            0x85 => ServerMsg::RoomState(get_room(&mut r)?),
            0x86 => ServerMsg::Datagram { src_ip: r.u32()?, src_port: r.u16()?, dst_ip: r.u32()?, dst_port: r.u16()?, data: r.blob(MAX_DATA)? },
            0x87 => ServerMsg::Left(r.u16()?),
            0x88 => ServerMsg::Pong(r.u32()?),
            _ => return Err(BadFrame),
        };
        r.end()?;
        Ok(m)
    }
}
