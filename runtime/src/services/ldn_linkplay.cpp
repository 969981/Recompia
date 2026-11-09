// SPDX-License-Identifier: GPL-3.0-or-later
// ldn: ldn:u (IUserServiceCreator -> IUserLocalCommunicationService), Nintendo local wireless, carried by a vnet::Lobby (multiplayer
// phase 2): a network the game creates is a room on the relay server, a scan lists the server's rooms, a connect joins one, and while the
// node is in a room its lobby link is the current vnet link, so the bsd UDP sockets reach the other nodes (169.254.R.N/24).
// Game-independent.
//
// Adapted from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Ldn/Types/* (NetworkInfo, CommonNetworkInfo, LdnNetworkInfo, NodeInfo,
//   NodeLatestUpdate, ScanFilter, NetworkConfig, SecurityConfig, SecurityParameter, UserConfig, IntentId, NetworkId), Ldn/ResultCode.cs,
//   Ldn/UserServiceCreator/IUserLocalCommunicationService.cs (raw argument layouts: CreateNetwork reads SecurityConfig, UserConfig, 4 pad,
//   NetworkConfig; Connect reads SecurityConfig, UserConfig, u32 version, u32 option with the NetworkInfo in buffer 0x19; Scan reads the
//   channel as 8 bytes, then the ScanFilter) and the DisconnectReason enum.
// Switchbrew "LDN services": the command table (Switch 2 ids 105, 403, 500..601 included), the State enum, the struct offsets, the state
//   transitions (Initialize None->Initialized, OpenAccessPoint 1->2, CreateNetwork 2->3, DestroyNetwork 3->2, CloseAccessPoint 2/3->1,
//   OpenStation 1->4, Connect 4->5, Disconnect 5->4, CloseStation 4/5->1, Finalize any->0), GetNetworkInfo needing state 3 or 5, Scan
//   needing 2..5, SetAdvertiseData <= 0x180 bytes in state 2..3, Reject in state 3.
//
// Mapping onto the lobby (lobby.h): SessionId = "pkpR" + u32 room id (so a NetworkInfo from Scan maps back to its room on Connect);
// BSSID = the host node's MAC (02:00:a9:fe:R:1); SSID = 32 hex digits from the local communication id and the room id; channel 1/6/11 by
// room id; link level 3; network type 2; the security parameter's random half = SHA-256 of the session id (host and guests agree). The
// passphrase reaches the server only as its truncated SHA-256 (vnet::passphrase_hash).
// Node table: the server's nicknames ([account] nickname of each player); the node's own entry uses the UserConfig name the game passed.
//
// When nobody leaves on purpose: the host closing the room or the server going away puts a station back to Station and a host back to
// AccessPoint with GetDisconnectReason set (DestroyedByUser 3 when the host destroyed the room, Rejected 5, DestroyedBySystem 4 on server
// shutdown, SignalLost 6 when the connection to the server is lost); the state change event is signalled on every state change and on
// every node join/leave.
// Host: the current link is a vnet::HoldLink in front of the lobby's, so datagrams from a node that just joined wait (up to kHoldMs) until
// the game has read a GetNetworkInfo / GetNetworkInfoLatestUpdate listing it; KTGL drops a sender outside its member table (design from
// pokopia-native (MIT) windows/src/coop/coop_mpnet.c, ACCEPT_WAIT_MS / cmp_host_accepts, @7c3655d).
// [net] room_code: the lobby's room key (vnet::room_key); a room hosted with one is hidden from players without it. A Connect refused for a
// wrong code answers ConnectNotFound.
// Session variant (services_api.h ldnapi): a game module's opt-in gameplay change that every player must share rides in bits 12..14 of the
// local communication version on the wire (CreateNetwork's room, Connect's JOIN), so the relay's own version check refuses a player whose
// variant differs. The game sees its own version only (NetworkInfo nodes, GetNetworkConfig). A Connect refused for another variant answers
// ConnectRejected and logs which side has what; the variant of a room comes from the last Scan that listed it.
// No lobby ([net] server empty or off): the state machine runs, Scan finds nothing, Connect answers ConnectNotFound and CreateNetwork
// DeviceNotAvailable (the answers of the offline stub this replaces).
// [net] server = auto (the default): no fixed relay. CreateNetwork starts the relay inside this process (vnet::RelayServer on 0.0.0.0:39660,
// once; it runs until the process exits; a port already taken = another relay on this PC, used as it is) and hosts on it through
// 127.0.0.1. Scan outside a room runs LAN discovery (vnet::discover_relays, at most every 2 s, the last result in between), asks every
// relay found for its rooms in parallel (one kept relay client per relay, at most 1.5 s for all of them) and lists the rooms of all of
// them: several hosts on one network each show up. The session id carries the room's relay (session_of: a tag from the relay's WELCOME
// instance id, the same for the host and its visitors), and the BSSID, SSID, node MACs, security parameter and authentication id are
// derived from it too, so two relays' room 1 never look alike. Connect moves the node to the relay its session id names, then joins.
// Moving = Ldn::adopt(): only while this node is in no room (no current link, no hold link, both point at the lobby in use), the old
// lobby's sinks are cleared before the new one's are set; the old relay client stays as a scan client. host:port mode writes a zero tag:
// every byte as before.
//
// That is the Link Play backend ([net] protocol = linkplay). The default, [net] protocol = native, is pokopia-native's co-op module
// (NativeLdn, ldn_native.cpp): wire-compatible with pokopia-native builds, not with the relay.
#include "ldn_linkplay.h"


#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "coop.h"
#include "../net/relay_server.h"
#include "../net/vnet.h"
#include "services_internal.h"

namespace nxrt::services::ldn_impl {

namespace {

SessionRef session_of(const uint8_t (&sid)[16]) {
    SessionRef s;
    if (std::memcmp(sid, kSessionTag, 4) != 0) return s;
    std::memcpy(&s.room, sid + 4, 4);
    for (int i = 0; i < 6; ++i) s.tag |= uint64_t{sid[8 + i]} << (8 * i);
    return s;
}

// A node MAC (02:00:a9:fe:R:N from the relay) with a relay tag: bytes 1..3 become the tag's first three, so two relays' room R differ in
// the BSSID and the node table too. Tag 0: unchanged.
std::array<uint8_t, 6> tagged_mac(std::array<uint8_t, 6> m, uint64_t tag) {
    if (tag)
        for (int i = 0; i < 3; ++i) m[static_cast<size_t>(1 + i)] = static_cast<uint8_t>(tag >> (8 * i));
    return m;
}

// Whether a room state lists the host (node 0) and the node with address vip.
bool room_lists(const vnet::Room& r, uint32_t vip) {
    bool host = false, me = false;
    for (const vnet::RoomNode& n : r.nodes) {
        host |= n.node_id == 0;
        me |= n.vip == vip;
    }
    return host && me;
}

// The NetworkInfo of a lobby room. own_vip/own_name: this node's entry uses the game's UserConfig name. tag: the room's relay (relay_tag(),
// every mode; 0 only for a relay without a version = the layout as before). Everything the game could use to tell networks apart carries it: session id,
// BSSID and node MACs (tagged_mac), SSID (tag << 16 | room id), the security parameter (SHA-256 of the session id) and the
// authentication id. The host and its visitors compute the same tag for their relay, so every node sees the same values.
NetworkInfo make_info(const vnet::Room& r, uint8_t accept_policy, uint32_t own_vip, const std::string& own_name, uint64_t tag) {
    NetworkInfo ni{};
    ni.id.intent.local_comm_id = r.local_comm_id;
    ni.id.intent.scene_id = r.scene_id;
    std::memcpy(ni.id.session_id, kSessionTag, 4);
    std::memcpy(ni.id.session_id + 4, &r.room_id, 4);
    for (int i = 0; i < 6; ++i) ni.id.session_id[8 + i] = static_cast<uint8_t>(tag >> (8 * i));
    const uint64_t net = tag ? (tag << 16) | (r.room_id & 0xFFFF) : r.room_id;
    const auto mac = tagged_mac({0x02, 0x00, 0xA9, 0xFE, static_cast<uint8_t>(r.room_id), 1}, tag);
    std::memcpy(ni.common.bssid, mac.data(), 6);
    char ssid[33];
    std::snprintf(ssid, sizeof ssid, "%016llx%016llx", static_cast<unsigned long long>(r.local_comm_id), static_cast<unsigned long long>(net));
    ni.common.ssid.length = 32;
    std::memcpy(ni.common.ssid.raw, ssid, 32);
    static constexpr int16_t kChannels[3] = {1, 6, 11};
    ni.common.channel = kChannels[r.room_id % 3];
    ni.common.link_level = 3;
    ni.common.network_type = 2;
    const auto sp = vnet::sha256(ni.id.session_id, sizeof ni.id.session_id);
    std::memcpy(ni.ldn.security_parameter, sp.data(), 16);
    ni.ldn.security_mode = r.security_mode;
    ni.ldn.station_accept_policy = accept_policy;
    ni.ldn.node_count_max = r.node_count_max;
    for (const vnet::RoomNode& n : r.nodes) {
        if (n.node_id >= 8) continue;
        NodeInfo& d = ni.ldn.nodes[n.node_id];
        d.ipv4 = n.vip;
        std::memcpy(d.mac, tagged_mac(n.mac, tag).data(), 6);
        d.node_id = static_cast<int8_t>(n.node_id);
        d.is_connected = 1;
        copy_name(d.user_name, n.vip == own_vip && !own_name.empty() ? own_name : n.nickname);
        d.local_comm_version = game_version(r.comm_version);
        ++ni.ldn.node_count;
    }
    const size_t adv = r.adv.size() < sizeof ni.ldn.advertise_data ? r.adv.size() : sizeof ni.ldn.advertise_data;
    ni.ldn.advertise_data_size = static_cast<uint16_t>(adv);
    if (adv) std::memcpy(ni.ldn.advertise_data, r.adv.data(), adv);
    ni.ldn.authentication_id = net;
    return ni;
}

bool filter_match(const ScanFilter& f, const NetworkInfo& ni) {
    if ((f.flag & kFilterSessionId) && std::memcmp(f.id.session_id, ni.id.session_id, 16) != 0) return false;
    if ((f.flag & kFilterNetworkType) && !(f.network_type & ni.common.network_type)) return false;
    if ((f.flag & kFilterBssid) && std::memcmp(f.bssid, ni.common.bssid, 6) != 0) return false;
    if ((f.flag & kFilterSsid) &&
        (f.ssid.length != ni.common.ssid.length || std::memcmp(f.ssid.raw, ni.common.ssid.raw, ni.common.ssid.length) != 0))
        return false;
    return true;   // local communication id and scene id were filtered by the server
}

}  // namespace

// =============================================================================================================================================
// One IUserLocalCommunicationService. op_mu serialises the commands (it is held across lobby requests); mu guards the fields and is the only
// lock the lobby's event sinks take, so a sink never waits on a request in flight.
// =============================================================================================================================================

void Ldn::cmd(ipc::Service& s, uint32_t id, const char* name, H h) {
    std::weak_ptr<Ldn> w = weak_from_this();
    s.add_cmd(id, name, [w, id, name, h](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(id, name);
        auto self = w.lock();
        if (!self) return kInvalidState;
        std::lock_guard<std::mutex> op(self->op_mu_);
        return ((*self).*h)(req, rsp);
    });
}

void Ldn::add_commands(ipc::Service& s) {
    cmd(s, 0, "GetState", &Ldn::get_state);
    cmd(s, 1, "GetNetworkInfo", &Ldn::get_network_info);
    cmd(s, 2, "GetIpv4Address", &Ldn::get_ipv4);
    cmd(s, 3, "GetDisconnectReason", &Ldn::get_disconnect_reason);
    cmd(s, 4, "GetSecurityParameter", &Ldn::get_security_parameter);
    cmd(s, 5, "GetNetworkConfig", &Ldn::get_network_config);
    cmd(s, 100, "AttachStateChangeEvent", &Ldn::attach_event);
    cmd(s, 101, "GetNetworkInfoLatestUpdate", &Ldn::get_latest_update);
    cmd(s, 102, "Scan", &Ldn::scan);
    cmd(s, 103, "ScanPrivate", &Ldn::scan);
    cmd(s, 104, "SetWirelessControllerRestriction", &Ldn::accept);
    cmd(s, 105, "SetWirelessAudioPolicy", &Ldn::accept);
    cmd(s, 106, "SetProtocol", &Ldn::accept);
    cmd(s, 200, "OpenAccessPoint", &Ldn::open_access_point);
    cmd(s, 201, "CloseAccessPoint", &Ldn::close_access_point);
    cmd(s, 202, "CreateNetwork", &Ldn::create_network);
    cmd(s, 203, "CreateNetworkPrivate", &Ldn::create_network_private);
    cmd(s, 204, "DestroyNetwork", &Ldn::destroy_network);
    cmd(s, 205, "Reject", &Ldn::reject);
    cmd(s, 206, "SetAdvertiseData", &Ldn::set_advertise_data);
    cmd(s, 207, "SetStationAcceptPolicy", &Ldn::set_accept_policy);
    cmd(s, 208, "AddAcceptFilterEntry", &Ldn::accept_filter);
    cmd(s, 209, "ClearAcceptFilter", &Ldn::accept_filter);
    cmd(s, 300, "OpenStation", &Ldn::open_station);
    cmd(s, 301, "CloseStation", &Ldn::close_station);
    cmd(s, 302, "Connect", &Ldn::connect);
    cmd(s, 303, "ConnectPrivate", &Ldn::connect_private);
    cmd(s, 304, "Disconnect", &Ldn::disconnect);
    cmd(s, 400, "Initialize", &Ldn::initialize);
    cmd(s, 401, "Finalize", &Ldn::finalize);
    cmd(s, 402, "InitializeWithVersion", &Ldn::initialize);
    cmd(s, 403, "SetOperationMode", &Ldn::accept);
    // 500..601 (action frames, home channel, tx power; Switchbrew, 18.0.0+) stay unregistered: the IPC layer logs them if a game asks.
}

void Ldn::admit_read() {
    if (!hold_) return;
    std::vector<uint32_t> v;
    for (const uint32_t ip : node_vip_)
        if (ip) v.push_back(ip);
    hold_->admit(v);
}

void Ldn::drop_hold() {
    if (!hold_) return;
    hold_->detach();   // before the lobby's link can be the current link again (a Connect as station)
    hold_.reset();
}

void Ldn::set_state(uint32_t s) {
    if (state_ != s) NXLOG_I("net", "ldn: state %u -> %u", state_, s);
    state_ = s;
    ev_.signal();
}

vnet::LobbyEvents Ldn::make_events() {
    std::weak_ptr<Ldn> w = weak_from_this();
    vnet::LobbyEvents ev;
    ev.room_state = [w](const vnet::Room& r) {
        if (auto s = w.lock()) s->on_room_state(r);
    };
    ev.left = [w](vnet::LeftReason why) {
        if (auto s = w.lock()) s->on_left(why);
    };
    return ev;
}

// What refers to a lobby: lobby_ (and the process slot, auto_relay()), its event sinks (set_events), the vnet current link (its link() for
// a station, hold_ in front of it for a host), hold_ itself, and the local copies the commands take under op_mu_. Outside a room the last
// three are clear (leave_network / on_left unhook the current link and drop hold_ before the state leaves 3/5), and the commands are
// serialised by op_mu_, which the caller holds, so no command has a copy in flight and none can enter a room meanwhile; sinks only ever
// take the state out of a room. So the move is refused while in a room, and otherwise: swap lobby_ under mu_, then (mu_ released, as a
// sink in progress may be waiting for it) clear the old lobby's sinks, which waits for a sink in progress and guarantees none runs after,
// and set the new one's. The old relay client stays in slot().relays as a scan client (no sinks: a client in no room gets no events)
// until no discovery lists it for 10 s. Scan results are kept per relay (scan_wire_, tag_relay_).
void Ldn::adopt(std::shared_ptr<vnet::Lobby> next) {
    std::shared_ptr<vnet::Lobby> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (lobby_ == next || in_network()) return;
        drop_hold();   // none outside a room
        old = std::move(lobby_);
        lobby_ = next;
    }
    if (old) {
        if (const auto cur = vnet::current_link(); cur && cur == old->link()) vnet::set_current_link(nullptr);   // none outside a room
        old->set_events({});
    }
    if (next) next->set_events(make_events());
}

// ---- lobby events (lobby thread) ------------------------------------------------------------------------------------------------------

void Ldn::on_room_state(const vnet::Room& r) {
    std::lock_guard<std::mutex> lk(mu_);
    room_cv_.notify_all();
    if (!in_network() || r.room_id != room_id_) return;
    apply_room(r);
    ev_.signal();
}

void Ldn::on_left(vnet::LeftReason why) {
    std::lock_guard<std::mutex> lk(mu_);
    room_cv_.notify_all();
    if (!in_network()) return;
    int16_t reason = kSignalLost;
    switch (why) {
        case vnet::LeftReason::HostClosed: reason = kDestroyedByUser; break;
        case vnet::LeftReason::Rejected: reason = kRejected; break;
        case vnet::LeftReason::Shutdown: reason = kDestroyedBySystem; break;
        default: reason = state_ == kAccessPointCreated ? kDestroyedBySystem : kSignalLost; break;
    }
    NXLOG_I("net", "ldn: left room %u (reason %d)", room_id_, reason);
    vnet::set_current_link(nullptr);
    drop_hold();
    reason_ = reason;
    room_id_ = vip_ = 0;
    info_ = NetworkInfo{};
    node_vip_ = {};
    set_state(state_ == kAccessPointCreated ? kAccessPoint : kStation);
}

void Ldn::apply_room(const vnet::Room& r) {
    info_ = make_info(r, accept_policy_, vip_, user_name_, tag_);
    std::array<uint32_t, 8> now{};
    for (const vnet::RoomNode& n : r.nodes)
        if (n.node_id < 8) now[n.node_id] = n.vip;
    for (size_t i = 0; i < 8; ++i) {
        if (now[i] == node_vip_[i]) continue;
        if (node_vip_[i]) latest_[i] |= kNodeDisconnect;
        if (now[i]) latest_[i] |= kNodeConnect;
    }
    node_vip_ = now;
    if (hold_) {
        std::vector<uint32_t> v;
        for (const uint32_t ip : now)
            if (ip) v.push_back(ip);
        hold_->members(v);
    }
}

// ---- state and info ---------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::get_state(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    rsp.push(state_);
    return 0;
}

uint32_t Ldn::get_network_info(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    buf_write(req.out_buf(0), &info_, sizeof info_);
    admit_read();
    return 0;
}

uint32_t Ldn::get_ipv4(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    rsp.push(vip_);
    rsp.push(kSubnetMask);
    return 0;
}

uint32_t Ldn::get_disconnect_reason(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    rsp.push(reason_);
    return 0;
}

uint32_t Ldn::get_security_parameter(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    SecurityParameter sp{};
    std::memcpy(sp.data, info_.ldn.security_parameter, 16);
    std::memcpy(sp.session_id, info_.id.session_id, 16);
    rsp.push(sp);
    return 0;
}

uint32_t Ldn::get_network_config(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    NetworkConfig c{};
    c.intent = info_.id.intent;
    c.channel = static_cast<uint16_t>(info_.common.channel);
    c.node_count_max = info_.ldn.node_count_max;
    c.local_comm_version = static_cast<uint16_t>(info_.ldn.nodes[0].local_comm_version);
    rsp.push(c);
    return 0;
}

uint32_t Ldn::attach_event(ipc::Request&, ipc::Response& rsp) {
    rsp.out_copy.push_back(ev_.new_handle());
    return 0;
}

uint32_t Ldn::get_latest_update(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    buf_write(req.out_buf(0), &info_, sizeof info_);
    NodeLatestUpdate up[8] = {};
    for (size_t i = 0; i < 8; ++i) up[i].state_change = latest_[i];
    buf_write(req.out_buf(1), up, sizeof up);
    latest_ = {};
    admit_read();
    return 0;
}

// ---- scan ----------------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::scan(ipc::Request& req, ipc::Response& rsp) {
    bool all;   // all: auto mode outside a room, the rooms of every relay on the LAN
    std::shared_ptr<vnet::Lobby> lobby;
    uint32_t own_room;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kNone) return kInvalidState;
        // With no lobby every initialized state answers an empty scan (the offline stub's answer); with one (or auto), Switchbrew's 2..5.
        if ((lobby_ || auto_) && (state_ < kAccessPoint || state_ > kStationConnected)) return kInvalidState;
        all = auto_ && !in_network();
        lobby = lobby_;
        own_room = room_id_;
    }
    const ScanFilter f = arg_at<ScanFilter>(req, 8);   // after the s16 channel, 8-aligned
    const ipc::Buffer out = req.out_buf(0);
    const size_t cap = static_cast<size_t>(out.size / sizeof(NetworkInfo));
    const uint64_t lcid = (f.flag & kFilterLocalCommunicationId) ? f.id.intent.local_comm_id : 0;
    const uint16_t scene = (f.flag & kFilterSceneId) ? f.id.intent.scene_id : 0xFFFF;
    uint16_t count = 0;
    auto put = [&](const vnet::Room& r, uint64_t tag) {
        if (count >= cap) return;
        const NetworkInfo ni = make_info(r, 0, 0, {}, tag);
        if (!filter_match(f, ni)) return;
        std::memcpy(buf_mut(out) + count * sizeof(NetworkInfo), &ni, sizeof ni);
        ++count;
    };
    if (all && cap) {
        // Every relay found, merged. One relay reached by two addresses (a relay on this PC answers on 127.0.0.1 and on its LAN address,
        // a PC on two networks on both) has one tag: it is listed once, by its loopback address if it has one.
        std::vector<RelayRooms> got = scan_relays(discover_cached(), lcid, scene);
        std::stable_sort(got.begin(), got.end(), [](const RelayRooms& a, const RelayRooms& b) { return a.loopback && !b.loopback; });
        std::set<uint64_t> tags;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (RelayRooms& rr : got) {
                if (rr.tag && !tags.insert(rr.tag).second) {
                    rr.rooms.clear();
                    continue;
                }
                tag_relay_[rr.tag] = {rr.host, rr.port};
                for (const vnet::Room& r : rr.rooms) scan_wire_[{rr.tag, r.room_id}] = r.comm_version;
            }
        }
        for (const RelayRooms& rr : got)
            for (const vnet::Room& r : rr.rooms) put(r, rr.tag);
    } else if (lobby && cap) {
        std::vector<vnet::Room> rooms;
        const int e = lobby->scan(lcid, scene, &rooms);
        if (e) NXLOG_D("net", "ldn: scan failed: %s", vnet::lobby_err_str(e));
        const uint64_t tag = relay_tag(*lobby);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const vnet::Room& r : rooms) scan_wire_[{tag, r.room_id}] = r.comm_version;
        }
        for (const vnet::Room& r : rooms)
            if (r.room_id != own_room) put(r, tag);
    }
    rsp.push(static_cast<uint32_t>(count));   // s16 total (the high half is zero)
    return 0;
}

// ---- access point ----------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::open_access_point(ipc::Request&, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kInitialized) return kInvalidState;
    set_state(kAccessPoint);
    return 0;
}

void Ldn::leave_network(uint32_t next, int16_t reason) {
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (in_network()) {
            vnet::set_current_link(nullptr);
            drop_hold();
            lobby = lobby_;
            room_id_ = vip_ = 0;
            info_ = NetworkInfo{};
            node_vip_ = {};
            reason_ = reason;
        }
        set_state(next);
    }
    if (lobby) lobby->leave();   // outside mu: the loopback relay may run sinks of this very object
}

uint32_t Ldn::close_access_point(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
    }
    leave_network(kInitialized, kDestroyedByUser);
    return 0;
}

uint32_t Ldn::create_network(ipc::Request& req, ipc::Response&) {
    return create(arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kCreateUser), arg_at<NetworkConfig>(req, kCreateNetCfg));
}

uint32_t Ldn::create_network_private(ipc::Request& req, ipc::Response&) {
    return create(arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kCreatePrivUser), arg_at<NetworkConfig>(req, kCreatePrivNetCfg));
}

uint32_t Ldn::create(const SecurityConfig& sec, const UserConfig& user, const NetworkConfig& cfg) {
    bool host_auto;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint) return kInvalidState;
        if (cfg.node_count_max < 1 || cfg.node_count_max > vnet::kMaxNodes || sec.passphrase_size > sizeof sec.passphrase)
            return kInvalidArgument;
        host_auto = auto_;
    }
    if (host_auto) adopt(host_relay());   // state 2 (no room) and op_mu_ held: nothing can enter one meanwhile
    std::shared_ptr<vnet::Lobby> lobby;
    vnet::RoomConfig rc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!lobby_) return kDeviceNotAvailable;
        lobby = lobby_;
        rc.local_comm_id = cfg.intent.local_comm_id;
        rc.scene_id = cfg.intent.scene_id;
        rc.comm_version = wire_version(static_cast<int16_t>(cfg.local_comm_version));
        rc.node_count_max = cfg.node_count_max;
        rc.security_mode = static_cast<uint8_t>(sec.mode);
        rc.pass_hash = vnet::passphrase_hash(sec.passphrase, sec.passphrase_size);
        rc.adv = adv_;
        user_name_.assign(user.user_name, strnlen(user.user_name, 32));
    }
    vnet::Joined j;
    const int e = lobby->create(rc, &j);
    if (e) {
        NXLOG_I("net", "ldn: CreateNetwork failed: %s", vnet::lobby_err_str(e));
        return kDeviceNotAvailable;
    }
    const uint64_t tag = relay_tag(*lobby);   // what visitors put in the session id for this relay (every mode: all nodes agree)
    std::lock_guard<std::mutex> lk(mu_);
    tag_ = tag;
    room_id_ = j.room_id;
    vip_ = j.vip;
    reason_ = kReasonNone;
    node_vip_ = {};
    latest_ = {};
    vnet::Room r = lobby->room();
    // No ROOM_STATE yet: the host alone. This is the whole room (node 0 = this node, connected, with the advertise data), so CreateNetwork
    // needs no wait; a ROOM_STATE that comes later replaces it.
    if (r.room_id != j.room_id || r.nodes.empty()) {
        r.room_id = j.room_id;
        r.local_comm_id = rc.local_comm_id;
        r.scene_id = rc.scene_id;
        r.comm_version = rc.comm_version;
        r.node_count_max = rc.node_count_max;
        r.security_mode = rc.security_mode;
        r.adv = rc.adv;
        r.nodes = {{0, j.vip, {0x02, 0x00, 0xA9, 0xFE, static_cast<uint8_t>(j.vip >> 8), static_cast<uint8_t>(j.vip)}, user_name_}};
    }
    drop_hold();
    hold_ = std::make_shared<vnet::HoldLink>(lobby->link(), kHoldMs, kHoldCap);
    apply_room(r);
    vnet::set_current_link(hold_);
    std::string what;
    if (const uint8_t var = variant_of(rc.comm_version); var && own_variant(&what) == var)
        NXLOG_I("net", "ldn: created room %u as %s with %s (session variant %u: only players with it can join)", room_id_,
                vnet::ip_str(vip_).c_str(), what.c_str(), var);
    else NXLOG_I("net", "ldn: created room %u as %s", room_id_, vnet::ip_str(vip_).c_str());
    set_state(kAccessPointCreated);
    return 0;
}

uint32_t Ldn::destroy_network(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPointCreated) return kInvalidState;
    }
    leave_network(kAccessPoint, kDestroyedByUser);
    return 0;
}

uint32_t Ldn::reject(ipc::Request& req, ipc::Response&) {
    const uint32_t ip = req.arg<uint32_t>(0);
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPointCreated) return kInvalidState;
        bool found = false;
        for (size_t i = 1; i < 8; ++i) found |= node_vip_[i] == ip && ip != 0;
        if (!found) return kNodeNotFound;
        lobby = lobby_;
    }
    lobby->reject(ip);   // the server answers with a ROOM_STATE without that node
    return 0;
}

uint32_t Ldn::set_advertise_data(ipc::Request& req, ipc::Response&) {
    const ipc::Buffer b = req.in_buf(0);
    if (b.size > vnet::kMaxAdvertise) return kInvalidArgument;
    std::shared_ptr<vnet::Lobby> lobby;
    std::vector<uint8_t> adv(buf_ptr(b), buf_ptr(b) + (b.addr ? b.size : 0));
    NXLOG_D("net", "ldn: SetAdvertiseData %zu bytes: %s", adv.size(), hex_head(adv).c_str());
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        adv_ = adv;
        if (state_ == kAccessPointCreated) {
            lobby = lobby_;
            info_.ldn.advertise_data_size = static_cast<uint16_t>(adv.size());
            std::memset(info_.ldn.advertise_data, 0, sizeof info_.ldn.advertise_data);
            if (!adv.empty()) std::memcpy(info_.ldn.advertise_data, adv.data(), adv.size());
        }
    }
    if (lobby) lobby->set_advertise(adv);
    return 0;
}

uint32_t Ldn::set_accept_policy(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
    accept_policy_ = req.arg<uint8_t>(0);   // stored and reported; the relay has no accept policy yet (everyone may join)
    info_.ldn.station_accept_policy = accept_policy_;
    return 0;
}

uint32_t Ldn::accept_filter(ipc::Request&, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    return state_ == kAccessPoint || state_ == kAccessPointCreated ? 0 : kInvalidState;
}

// ---- station ----------------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::open_station(ipc::Request&, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kInitialized) return kInvalidState;
    set_state(kStation);
    return 0;
}

uint32_t Ldn::close_station(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kStation && state_ != kStationConnected) return kInvalidState;
    }
    leave_network(kInitialized, kDisconnectedByUser);
    return 0;
}

uint32_t Ldn::connect(ipc::Request& req, ipc::Response&) {
    NetworkInfo ni{};
    buf_read(req.in_buf(0), &ni, sizeof ni);
    return join(session_of(ni.id.session_id), ni.ldn.nodes[0].local_comm_version, arg_at<SecurityConfig>(req, 0),
                arg_at<UserConfig>(req, kConnectUser), req.arg<int32_t>(kConnectVersion));
}

uint32_t Ldn::connect_private(ipc::Request& req, ipc::Response&) {
    const SecurityParameter sp = arg_at<SecurityParameter>(req, kConnPrivSecParam);
    const NetworkConfig cfg = arg_at<NetworkConfig>(req, kConnPrivNetCfg);
    return join(session_of(sp.session_id), static_cast<int16_t>(cfg.local_comm_version), arg_at<SecurityConfig>(req, 0),
                arg_at<UserConfig>(req, kConnPrivUser), req.arg<int32_t>(kConnPrivVersion));
}

uint32_t Ldn::join(const SessionRef& sref, int16_t room_version, const SecurityConfig& sec, const UserConfig& user, int32_t version) {
    const uint32_t room_id = sref.room;
    bool is_auto;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kNone) return kInvalidState;
        is_auto = auto_;
    }
    // Auto mode: the session id names the room's relay; the node moves to that relay's client first (state 4, no room: adopt() is safe).
    // A tag no Scan has listed (another PC's session id, a relay forgotten since): not found, rather than the same room id on another
    // relay. Tag 0: the lobby in use.
    if (is_auto && sref.tag && room_id) {
        std::pair<std::string, uint16_t> where;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStation) return kInvalidState;
            const auto it = tag_relay_.find(sref.tag);
            if (it == tag_relay_.end()) {
                NXLOG_I("net", "ldn: Connect to room %u: its relay is not one a Scan found", room_id);
                return kConnectNotFound;
            }
            where = it->second;
        }
        adopt(auto_relay(where.first, where.second));
    }
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!lobby_) return kConnectNotFound;   // no radio: nothing to connect to (the offline stub's answer, any state)
        if (state_ != kStation) return kInvalidState;
        if (sec.passphrase_size > sizeof sec.passphrase) return kInvalidArgument;
        lobby = lobby_;
        user_name_.assign(user.user_name, strnlen(user.user_name, 32));
    }
    if (room_id == 0) return kConnectNotFound;
    const uint64_t tag = relay_tag(*lobby);
    if (is_auto && sref.tag && tag != sref.tag) {   // the relay at that address restarted (a new instance id): its rooms are gone
        NXLOG_I("net", "ldn: Connect to room %u: its relay is not the one the Scan listed any more", room_id);
        return kConnectNotFound;
    }
    vnet::Joined j;
    const int16_t wire = wire_version(static_cast<int16_t>(version));
    const int e = lobby->join(room_id, wire, vnet::passphrase_hash(sec.passphrase, sec.passphrase_size), &j);
    if (e) {
        NXLOG_I("net", "ldn: Connect to room %u failed: %s", room_id, vnet::lobby_err_str(e));
        if (e == vnet::kLobbyVersion) {
            int16_t room_wire = room_version;
            bool known = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const auto it = scan_wire_.find({tag, room_id});
                if (it != scan_wire_.end()) room_wire = it->second, known = true;
            }
            std::string what;
            const uint8_t mine = own_variant(&what), theirs = variant_of(room_wire);
            if (known && mine != theirs && game_version(room_wire) == game_version(wire)) {
                // the same game, another session variant: every player needs the same setting
                const char* name = what.empty() ? "a gameplay setting" : what.c_str();
                if (mine && theirs) NXLOG_W("net", "ldn: Connect to room %u refused: its session variant is %u, this PC's %u (%s)", room_id, theirs, mine, name);
                else if (mine) NXLOG_W("net", "ldn: Connect to room %u refused: the host has %s off, this PC has it on. Every player of a session needs the same setting", room_id, name);
                else NXLOG_W("net", "ldn: Connect to room %u refused: the host has %s on (session variant %u), this PC has it off. Every player of a session needs the same setting", room_id, name, theirs);
                return kConnectRejected;
            }
        }
        switch (e) {
            case vnet::kLobbyNoRoom:
            case vnet::kLobbyRoomCode: return kConnectNotFound;   // a room with another code: as if it were not there
            case vnet::kLobbyRoomFull: return kTooManyPlayers;
            case vnet::kLobbyVersion: return version < room_version ? kVersionTooLow : kVersionTooHigh;
            case vnet::kLobbyRejected: return kConnectRejected;
            case vnet::kLobbyTimeout: return kConnectTimeout;
            default: return kConnectFailure;   // wrong passphrase, no server
        }
    }
    // JOINED carries only the room id and this node's address; the node table and the advertise data come in the ROOM_STATE behind it.
    // The game reads both as soon as Connect returns (GetNetworkInfo: node[0] is the host it sends to, isConnected and the advertise data
    // with the host's session member id), so Connect returns only once the room state that lists the host and this node has been applied.
    // The room events notify room_cv_; the 50 ms slices also catch a state that came without an event. No such state within
    // kRoomStateWaitMs, or the lobby left the room meanwhile (its room() is then cleared): leave and fail like a failed join.
    std::unique_lock<std::mutex> lk(mu_);
    vnet::Room r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRoomStateWaitMs);
    for (;;) {
        r = lobby->room();
        if (r.room_id != j.room_id || room_lists(r, j.vip) || std::chrono::steady_clock::now() >= deadline) break;
        room_cv_.wait_for(lk, std::chrono::milliseconds(50));
    }
    if (r.room_id != j.room_id || !room_lists(r, j.vip)) {
        lk.unlock();
        NXLOG_I("net", "ldn: Connect to room %u: %s, leaving", j.room_id,
                r.room_id != j.room_id ? "out of the room before its state came" : "no room state listing this node within 5 s");
        lobby->leave();
        return kConnectFailure;
    }
    tag_ = relay_tag(*lobby);   // after JOIN the WELCOME is in: the same value the host computed
    room_id_ = j.room_id;
    vip_ = j.vip;
    reason_ = kReasonNone;
    node_vip_ = {};
    latest_ = {};
    apply_room(r);
    vnet::set_current_link(lobby->link());
    NXLOG_I("net", "ldn: joined room %u as %s (node %u)", room_id_, vnet::ip_str(vip_).c_str(), j.node_id);
    set_state(kStationConnected);
    return 0;
}

uint32_t Ldn::disconnect(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kStationConnected) return kInvalidState;
    }
    leave_network(kStation, kDisconnectedByUser);
    return 0;
}

// ---- lifetime ---------------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::initialize(ipc::Request&, ipc::Response&) {
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kNone) return 0;   // already initialized: accepted
        if (!offline_) lobby_ = process_lobby(&auto_);   // auto: the relay picked last in this process, or none yet
        lobby = lobby_;
        reason_ = kReasonNone;
        set_state(kInitialized);
    }
    if (!lobby) return 0;
    lobby->set_events(make_events());
    // Connect now, off the game's thread (Initialize runs at start-up): the first Scan/Create then finds the connection up, or waits for it.
    if (!lobby->connected()) std::thread([lobby] { lobby->connect(); }).detach();
    return 0;
}

uint32_t Ldn::finalize(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kNone) return kInvalidState;
    }
    shutdown();
    return 0;
}

void Ldn::shutdown() {
    leave_network(kNone, kDisconnectedByUser);
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lobby.swap(lobby_);
    }
    if (lobby) lobby->set_events({});
}

}  // namespace nxrt::services::ldn_impl
