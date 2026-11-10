// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u on the Link Play relay lobby ([net] protocol = linkplay): one IUserLocalCommunicationService (ldn_linkplay.cpp).
#pragma once
#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "../net/lobby.h"
#include "../net/vnet.h"
#include "ldn_backend.h"
#include "ldn_internal.h"

namespace nxrt::services::ldn_impl {

// SessionId = "pkpR" + u32 room id + 6 B relay tag + 2 zero bytes. The tag (relay_tag()) is set in every mode, so a host:port host and
// an auto visitor on one relay agree; zero only when the relay sent no version.
struct SessionRef {
    uint32_t room = 0;   // 0: not one of ours
    uint64_t tag = 0;
};

class Ldn final : public LdnSession, public std::enable_shared_from_this<Ldn> {
public:
    explicit Ldn(bool offline = false) : offline_(offline) {}
    void add_commands(ipc::Service& s);
    void shutdown() override;   // the service object was released: leave and drop the link

private:
    using H = uint32_t (Ldn::*)(ipc::Request&, ipc::Response&);
    void cmd(ipc::Service& s, uint32_t id, const char* name, H h);

    uint32_t get_state(ipc::Request&, ipc::Response&);
    uint32_t get_network_info(ipc::Request&, ipc::Response&);
    uint32_t get_ipv4(ipc::Request&, ipc::Response&);
    uint32_t get_disconnect_reason(ipc::Request&, ipc::Response&);
    uint32_t get_security_parameter(ipc::Request&, ipc::Response&);
    uint32_t get_network_config(ipc::Request&, ipc::Response&);
    uint32_t attach_event(ipc::Request&, ipc::Response&);
    uint32_t get_latest_update(ipc::Request&, ipc::Response&);
    uint32_t scan(ipc::Request&, ipc::Response&);
    uint32_t accept(ipc::Request&, ipc::Response&) { return 0; }
    uint32_t open_access_point(ipc::Request&, ipc::Response&);
    uint32_t close_access_point(ipc::Request&, ipc::Response&);
    uint32_t create_network(ipc::Request&, ipc::Response&);
    uint32_t create_network_private(ipc::Request&, ipc::Response&);
    uint32_t destroy_network(ipc::Request&, ipc::Response&);
    uint32_t reject(ipc::Request&, ipc::Response&);
    uint32_t set_advertise_data(ipc::Request&, ipc::Response&);
    uint32_t set_accept_policy(ipc::Request&, ipc::Response&);
    uint32_t accept_filter(ipc::Request&, ipc::Response&);
    uint32_t open_station(ipc::Request&, ipc::Response&);
    uint32_t close_station(ipc::Request&, ipc::Response&);
    uint32_t connect(ipc::Request&, ipc::Response&);
    uint32_t connect_private(ipc::Request&, ipc::Response&);
    uint32_t disconnect(ipc::Request&, ipc::Response&);
    uint32_t initialize(ipc::Request&, ipc::Response&);
    uint32_t finalize(ipc::Request&, ipc::Response&);

    uint32_t create(const SecurityConfig& sec, const UserConfig& user, const NetworkConfig& cfg);
    uint32_t join(const SessionRef& sref, int16_t room_version, const SecurityConfig& sec, const UserConfig& user, int32_t version);
    void on_room_state(const vnet::Room& r);
    void on_left(vnet::LeftReason why);
    void apply_room(const vnet::Room& r);       // caller holds mu
    void set_state(uint32_t s);                 // caller holds mu
    void admit_read();                          // caller holds mu: the game read info_, let its nodes' held datagrams through
    void drop_hold();                           // caller holds mu: the host's hold link is unhooked and gone
    void leave_network(uint32_t next, int16_t reason);   // caller holds op_mu (not mu)
    vnet::LobbyEvents make_events();
    void adopt(std::shared_ptr<vnet::Lobby> next);       // caller holds op_mu (not mu): auto mode moves to another relay
    bool in_network() const { return state_ == kAccessPointCreated || state_ == kStationConnected; }

    std::mutex op_mu_;
    std::mutex mu_;
    std::condition_variable room_cv_;    // notified (under mu_) on every lobby event; join() waits on it for its first room state
    ServiceEvent ev_;
    uint32_t state_ = kNone;
    int16_t reason_ = kReasonNone;
    std::shared_ptr<vnet::Lobby> lobby_;
    bool auto_ = false;                  // [net] server = auto (set by Initialize): lobby_ follows host_relay() / Connect
    std::vector<uint8_t> adv_;          // host: SetAdvertiseData before or after CreateNetwork
    uint8_t accept_policy_ = 0;
    std::string user_name_;              // the UserConfig name of the last create/connect
    uint32_t room_id_ = 0, vip_ = 0;
    NetworkInfo info_{};
    std::array<uint32_t, 8> node_vip_{};  // the node table last seen (0 = empty slot), for the join/leave flags
    std::array<uint8_t, 8> latest_{};    // NodeLatestUpdate flags since the last GetNetworkInfoLatestUpdate
    std::shared_ptr<vnet::HoldLink> hold_;   // host: the current link, in front of the lobby's (see kHoldMs)
    std::map<std::pair<uint64_t, uint32_t>, int16_t> scan_wire_;   // (relay tag, room id) -> its version on the wire as the last Scan
                                                                    // listed it (the session variant)
    std::map<uint64_t, std::pair<std::string, uint16_t>> tag_relay_;   // auto: relay tag -> its address, from the Scans so far
    uint64_t tag_ = 0;                   // the relay tag of the room this node is in
    bool offline_ = false;               // no lobby ever ([net] protocol = native with the co-op module off): the offline answers
};

}  // namespace nxrt::services::ldn_impl
