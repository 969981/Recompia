// SPDX-License-Identifier: GPL-3.0-or-later
// ldn over a lobby (multiplayer phase 2). The service under test is the process's ldn:u, driven through IPC; the other player is a lobby
// client used directly (one process has one ldn service and one bsd socket table). The request bytes and the offsets checked in the
// replies are written out from Switchbrew "LDN services" (NetworkInfo 0x480, NodeInfo 0x40 ...), not read from the service.
// ldn_live_relay runs two RelayClients through a real relay server when NXRT_TEST_RELAY=host:port is set (launcher: --host --port N).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/net/lobby.h"
#include "../src/net/vnet.h"
#include "../src/net/vsock.h"
#include "../src/services/ldn.h"
#include "nxrt/svc.h"
#include "svc_test_util.h"

NXTEST_GROUP(ldn)

using namespace nxrt;

namespace {
constexpr uint64_t kLcid = 0x0100123456789000ull;
constexpr int16_t kVersion = 3;
constexpr uint16_t kPort = 49530;

// ldn results: 203 | (description << 9)
constexpr uint32_t kInvalidState = 203 | (32 << 9);
constexpr uint32_t kNodeNotFound = 203 | (48 << 9);
constexpr uint32_t kConnectFailure = 203 | (64 << 9);
constexpr uint32_t kConnectNotFound = 203 | (65 << 9);
constexpr uint32_t kConnectRejected = 203 | (67 << 9);
constexpr uint32_t kVersionTooLow = 203 | (113 << 9);
constexpr uint32_t kDeviceNotAvailable = 203 | (16 << 9);

// NetworkInfo offsets (Switchbrew): NetworkId 0x00 (IntentId: u64 lcid, u16 scene at 0x0A; SessionId 0x10), CommonNetworkInfo 0x20
// (bssid 0x20, ssid len 0x26, channel 0x48, link level 0x4A, network type 0x4B), LdnNetworkInfo 0x50 (security mode +0x10, node count max
// +0x16, node count +0x17, nodes +0x18 x 0x40 {ipv4 0, mac 4, node id 0xA, connected 0xB, name 0xC, version 0x2E}, adv size +0x21A,
// adv +0x21C).
constexpr size_t kNi = 0x480, kLdn = 0x50, kNodes = kLdn + 0x18;

template <class T>
T at(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    std::memcpy(&v, b.data() + off, sizeof v);
    return v;
}
template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof v);
}

const uint8_t* pass() {
    static uint8_t p[64];
    for (int i = 0; i < 64; ++i) p[i] = static_cast<uint8_t>(0x40 + i);
    return p;
}

// SecurityConfig (0x44): u16 mode, u16 passphrase size, 64 B passphrase.
std::vector<uint8_t> security_config(uint8_t first_byte_xor = 0) {
    std::vector<uint8_t> b(0x44, 0);
    put<uint16_t>(b, 0, 1);
    put<uint16_t>(b, 2, 64);
    std::memcpy(b.data() + 4, pass(), 64);
    b[4] ^= first_byte_xor;
    return b;
}
// UserConfig (0x30): 33 B user name, 15 reserved.
std::vector<uint8_t> user_config(const char* name) {
    std::vector<uint8_t> b(0x30, 0);
    std::memcpy(b.data(), name, std::strlen(name));
    return b;
}
// CreateNetwork raw input: SecurityConfig at 0, UserConfig at 0x44, NetworkConfig at 0x78 (IntentId {lcid, scene at 0x0A}, channel 0x10,
// node count max 0x12, version 0x14).
std::vector<uint8_t> create_args(uint8_t node_max) {
    std::vector<uint8_t> b(0x98, 0);
    const auto sc = security_config();
    const auto uc = user_config("HostUser");
    std::memcpy(b.data(), sc.data(), sc.size());
    std::memcpy(b.data() + 0x44, uc.data(), uc.size());
    put<uint64_t>(b, 0x78, kLcid);
    put<uint16_t>(b, 0x78 + 0x0A, 0);   // scene 0
    put<uint8_t>(b, 0x78 + 0x12, node_max);
    put<int16_t>(b, 0x78 + 0x14, kVersion);
    return b;
}
// Connect raw input: SecurityConfig at 0, UserConfig at 0x44, s32 version at 0x74, u32 option at 0x78.
std::vector<uint8_t> connect_args(int32_t version, uint8_t pass_xor = 0) {
    std::vector<uint8_t> b(0x7C, 0);
    const auto sc = security_config(pass_xor);
    const auto uc = user_config("GuestUser");
    std::memcpy(b.data(), sc.data(), sc.size());
    std::memcpy(b.data() + 0x44, uc.data(), uc.size());
    put<int32_t>(b, 0x74, version);
    return b;
}

vnet::RoomConfig room_config() {
    vnet::RoomConfig c;
    c.local_comm_id = kLcid;
    c.scene_id = 0;
    c.comm_version = kVersion;
    c.node_count_max = 4;
    c.security_mode = 1;
    c.pass_hash = vnet::passphrase_hash(pass(), 64);
    c.adv = {'a', 'd', 'v'};
    return c;
}

bool signalled(uint32_t h) {
    int32_t idx = -1;
    return kernel::wait_synchronization(&h, 1, 0, &idx) == 0;
}
void reset_signal(uint32_t h) {   // svc ResetSignal (0x17), as the game clears the state change event
    ::Ctx c{};
    c.x[0] = h;
    svc_dispatch(&c, 0x17);
}

nxsvc::Session open_ldn() {
    nxsvc::Session ldn = nxsvc::connect("ldn:u");
    return ldn.sub(ldn.call(0));   // CreateUserLocalCommunicationService (0)
}
uint32_t state(nxsvc::Session& l) { return l.call(0).get<uint32_t>(0); }

// A bsd UDP socket on :49530 through IPC (the game's sequence): Socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK), SO_BROADCAST, Bind.
int32_t game_socket(nxsvc::Session& bsd) {
    const int32_t fd = bsd.call(2, nxsvc::Args().i32(2).i32(0x20000002).i32(0)).get<int32_t>(0);
    const int32_t one = 1;
    bsd.call(21, nxsvc::Args().i32(fd).i32(0xFFFF).i32(0x20).buf_a(&one, 4));
    const uint8_t any[16] = {16, 2, 0xC1, 0x7A};
    bsd.call(13, nxsvc::Args().i32(fd).buf_a(any, 16));
    return fd;
}

// A lobby whose room state comes late, the gap a relay leaves between JOINED and the ROOM_STATE behind it: join() succeeds at once, but
// until delay_ms has passed room() lists no node and has no advertise data, and the room_state event is held back. delay_ms < 0: the state
// never comes.
class LateRoomState final : public vnet::Lobby {
public:
    LateRoomState(std::shared_ptr<vnet::Lobby> in, int delay_ms) : in_(std::move(in)), delay_ms_(delay_ms) {}
    ~LateRoomState() override {
        if (t_.joinable()) t_.join();
    }
    void set_delay(int ms) { delay_ms_ = ms; }

    int connect() override { return in_->connect(); }
    bool connected() const override { return in_->connected(); }
    int scan(uint64_t lcid, uint16_t scene, std::vector<vnet::Room>* out) override { return in_->scan(lcid, scene, out); }
    int create(const vnet::RoomConfig& c, vnet::Joined* out) override { return in_->create(c, out); }
    int join(uint32_t id, int16_t ver, const std::array<uint8_t, 16>& hash, vnet::Joined* out) override {
        if (t_.joinable()) t_.join();
        held_ = true;
        const int e = in_->join(id, ver, hash, out);
        if (e) {
            held_ = false;
            return e;
        }
        const int delay = delay_ms_;
        if (delay >= 0) {
            t_ = std::thread([this, delay] {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                held_ = false;
                vnet::LobbyEvents ev;
                {
                    std::lock_guard<std::mutex> lk(ev_mu_);
                    ev = ev_;
                }
                if (ev.room_state && in_->in_room()) ev.room_state(in_->room());
            });
        }
        return e;
    }
    void leave() override { in_->leave(); }
    int set_advertise(const std::vector<uint8_t>& adv) override { return in_->set_advertise(adv); }
    int reject(uint32_t vip) override { return in_->reject(vip); }
    vnet::Room room() const override {
        vnet::Room r = in_->room();
        if (held_) {
            r.nodes.clear();
            r.adv.clear();
        }
        return r;
    }
    bool in_room() const override { return in_->in_room(); }
    std::shared_ptr<vnet::Link> link() override { return in_->link(); }
    void set_events(vnet::LobbyEvents ev) override {
        {
            std::lock_guard<std::mutex> lk(ev_mu_);
            ev_ = std::move(ev);
        }
        vnet::LobbyEvents fwd;
        fwd.room_state = [this](const vnet::Room& r) {
            if (held_) return;
            vnet::LobbyEvents e;
            {
                std::lock_guard<std::mutex> lk(ev_mu_);
                e = ev_;
            }
            if (e.room_state) e.room_state(r);
        };
        fwd.left = [this](vnet::LeftReason why) {
            vnet::LobbyEvents e;
            {
                std::lock_guard<std::mutex> lk(ev_mu_);
                e = ev_;
            }
            if (e.left) e.left(why);
        };
        in_->set_events(std::move(fwd));
    }

private:
    std::shared_ptr<vnet::Lobby> in_;
    std::atomic<int> delay_ms_;
    std::atomic<bool> held_{false};
    std::thread t_;
    std::mutex ev_mu_;
    vnet::LobbyEvents ev_;
};

template <class F>
bool wait_until(F&& f, int ms = 3000) {
    for (int i = 0; i < ms / 10; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}
}  // namespace

NXTEST(sha256_and_passphrase_hash) {
    // FIPS 180-2 example: SHA-256("abc")
    const auto h = vnet::sha256("abc", 3);
    const uint8_t want[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                              0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    CHECK(std::memcmp(h.data(), want, 32) == 0);
    // Two-block message (56 bytes): 248d6a61 d20638b8 e5c02693 0c3e6039 a33ce459 64ff2167 f6ecedd4 19db06c1
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const auto h2 = vnet::sha256(m, std::strlen(m));
    CHECK_EQ(h2[0], 0x24);
    CHECK_EQ(h2[31], 0xc1);
    const auto ph = vnet::passphrase_hash("abc", 3);
    CHECK(std::memcmp(ph.data(), want, 16) == 0);
}

NXTEST(parse_server_address) {
    std::string h;
    uint16_t p = 0;
    CHECK(vnet::parse_server("1.2.3.4:39670", &h, &p));
    CHECK(h == "1.2.3.4");
    CHECK_EQ(p, 39670);
    CHECK(vnet::parse_server(" pkp-host ", &h, &p));
    CHECK(h == "pkp-host");
    CHECK_EQ(p, 39660);
    CHECK(!vnet::parse_server("", &h, &p));
    CHECK(!vnet::parse_server("host:0", &h, &p));
    CHECK(!vnet::parse_server("host:x", &h, &p));
    CHECK(!vnet::parse_server(":39660", &h, &p));
}

// [net] server empty: the state machine runs, nothing is found, CreateNetwork has no device (the offline answers).
NXTEST(ldn_without_server) {
    nxsvc::Env env;
    nxsvc::Session l = open_ldn();
    REQUIRE(l.valid());
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid().u64(0)).result, 0u);
    CHECK_EQ(l.call(200).result, 0u);                         // OpenAccessPoint: 1 -> 2
    CHECK_EQ(state(l), 2u);
    CHECK_EQ(l.call(202, nxsvc::Args().bytes(create_args(4).data(), 0x98)).result, kDeviceNotAvailable);
    CHECK_EQ(l.call(1).result, kInvalidState);                // GetNetworkInfo: no network
    CHECK_EQ(l.call(201).result, 0u);                         // CloseAccessPoint
    CHECK_EQ(l.call(300).result, 0u);                         // OpenStation
    std::vector<uint8_t> nis(4 * kNi);
    std::vector<uint8_t> filter(0x60, 0);
    nxsvc::Reply sc = l.call(102, nxsvc::Args().u16(0).pad(6).bytes(filter.data(), 0x60).buf_b(nis.data(), nis.size()));
    CHECK_EQ(sc.result, 0u);
    CHECK_EQ(sc.get<uint16_t>(0), 0);
    std::vector<uint8_t> ni(kNi, 0);
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(ni.data(), kNi)).result, kConnectNotFound);
    CHECK_EQ(l.call(401).result, 0u);
    CHECK_EQ(state(l), 0u);
}

// The process hosts: Initialize, OpenAccessPoint, SetAdvertiseData, CreateNetwork; a lobby client scans, joins, exchanges datagrams with
// the game's bsd socket, is rejected; a second one joins and sees the host destroy the network.
NXTEST(ldn_host_over_loopback) {
    nxsvc::Env env;
    vnet::LoopbackRelay relay;
    services::ldn_set_lobby(relay.client("Josh"));
    auto guest = relay.client("Legolas");
    vnet::LeftReason guest_left = vnet::LeftReason::YouLeft;
    int guest_lefts = 0;
    guest->set_events({nullptr, [&](vnet::LeftReason r) { guest_left = r; ++guest_lefts; }});

    nxsvc::Session l = open_ldn();
    REQUIRE(l.valid());
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid().u64(0)).result, 0u);   // Initialize(pid, u64)
    nxsvc::Reply er = l.call(100);                                          // AttachStateChangeEvent
    REQUIRE(er.copy.size() == 1);
    const uint32_t ev = er.copy[0];
    reset_signal(ev);
    CHECK_EQ(l.call(200).result, 0u);                                      // OpenAccessPoint
    CHECK_EQ(state(l), 2u);
    CHECK(signalled(ev));
    reset_signal(ev);
    const char adv[] = "advert-1";
    CHECK_EQ(l.call(206, nxsvc::Args().buf_a(adv, 8)).result, 0u);       // SetAdvertiseData (0x21)
    CHECK_EQ(l.call(207, nxsvc::Args().u8(0)).result, 0u);               // SetStationAcceptPolicy
    CHECK_EQ(l.call(202, nxsvc::Args().bytes(create_args(4).data(), 0x98)).result, 0u);
    CHECK_EQ(state(l), 3u);
    CHECK(signalled(ev));
    reset_signal(ev);
    nxsvc::Reply ip = l.call(2);                                           // GetIpv4Address -> (address, mask)
    CHECK_EQ(ip.get<uint32_t>(0), vnet::ip(169, 254, 1, 1));
    CHECK_EQ(ip.get<uint32_t>(4), 0xFFFFFF00u);

    // The other player scans and finds the room with the advertise data and the host's node.
    std::vector<vnet::Room> rooms;
    CHECK_EQ(guest->scan(kLcid, 0xFFFF, &rooms), 0);
    REQUIRE(rooms.size() == 1);
    CHECK_EQ(rooms[0].comm_version, kVersion);
    CHECK_EQ(rooms[0].node_count_max, 4);
    REQUIRE(rooms[0].nodes.size() == 1);
    CHECK(rooms[0].nodes[0].nickname == "Josh");
    CHECK(rooms[0].adv == std::vector<uint8_t>(adv, adv + 8));
    std::vector<vnet::Room> none;
    CHECK_EQ(guest->scan(kLcid + 1, 0xFFFF, &none), 0);
    CHECK(none.empty());
    vnet::Joined j;
    CHECK_EQ(guest->join(rooms[0].room_id, kVersion, vnet::passphrase_hash(pass(), 64), &j), 0);
    CHECK_EQ(j.vip, vnet::ip(169, 254, 1, 2));
    CHECK_EQ(j.node_id, 1);
    CHECK(signalled(ev));                                                  // node join
    reset_signal(ev);

    // GetNetworkInfoLatestUpdate (NetworkInfo 0x1a, NodeLatestUpdate[8] 0x1a): two nodes, both flagged Connect, cleared on read.
    std::vector<uint8_t> ni(kNi), up(64);
    CHECK_EQ(l.call(101, nxsvc::Args().buf_c(ni.data(), kNi).buf_c(up.data(), 64)).result, 0u);
    CHECK_EQ(at<uint64_t>(ni, 0), kLcid);
    CHECK(std::memcmp(ni.data() + 0x10, "pkpR", 4) == 0);
    const uint8_t bssid[6] = {0x02, 0x00, 0xA9, 0xFE, 1, 1};
    CHECK(std::memcmp(ni.data() + 0x20, bssid, 6) == 0);
    CHECK_EQ(ni[0x26], 32);
    CHECK_EQ(ni[0x4A], 3);
    CHECK_EQ(ni[0x4B], 2);
    CHECK_EQ(at<uint16_t>(ni, kLdn + 0x10), 1);
    CHECK_EQ(ni[kLdn + 0x16], 4);
    CHECK_EQ(ni[kLdn + 0x17], 2);
    CHECK_EQ(at<uint32_t>(ni, kNodes), vnet::ip(169, 254, 1, 1));
    CHECK(std::strcmp(reinterpret_cast<const char*>(ni.data() + kNodes + 0xC), "HostUser") == 0);   // own node: the UserConfig name
    CHECK_EQ(at<uint32_t>(ni, kNodes + 0x40), vnet::ip(169, 254, 1, 2));
    CHECK_EQ(ni[kNodes + 0x40 + 0xA], 1);
    CHECK_EQ(ni[kNodes + 0x40 + 0xB], 1);
    CHECK(std::strcmp(reinterpret_cast<const char*>(ni.data() + kNodes + 0x40 + 0xC), "Legolas") == 0);
    CHECK_EQ(at<int16_t>(ni, kNodes + 0x40 + 0x2E), kVersion);
    CHECK_EQ(at<uint16_t>(ni, kLdn + 0x21A), 8);
    CHECK(std::memcmp(ni.data() + kLdn + 0x21C, adv, 8) == 0);
    CHECK_EQ(up[0], 1);
    CHECK_EQ(up[8], 1);
    CHECK_EQ(up[16], 0);
    CHECK_EQ(l.call(101, nxsvc::Args().buf_c(ni.data(), kNi).buf_c(up.data(), 64)).result, 0u);
    CHECK_EQ(up[0] | up[8], 0);
    std::vector<uint8_t> ni1(kNi);
    CHECK_EQ(l.call(1, nxsvc::Args().buf_c(ni1.data(), kNi)).result, 0u);   // GetNetworkInfo: the same table
    CHECK_EQ(ni1[kLdn + 0x17], 2);

    // UDP: the guest's socket (its own table on its lobby link) -> the game's bsd socket, and the host's broadcast back.
    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    const int32_t fd = game_socket(bsd);
    vsock::Stack gs(guest->link());
    const int gfd = gs.socket(2, 2, 0).ret;
    REQUIRE(gs.bind(gfd, {0, kPort}).ret == 0);
    const vsock::Addr host_addr{vnet::ip(169, 254, 1, 1), kPort};
    CHECK_EQ(gs.sendto(gfd, "hi!", 3, 0, &host_addr).ret, 3);
    std::vector<uint8_t> rbuf(0x1000);
    uint8_t sa[16] = {};
    nxsvc::Reply rf = bsd.call(9, nxsvc::Args().i32(fd).i32(0x80).buf_b(rbuf.data(), 0x1000).buf_b(sa, 16));
    CHECK_EQ(rf.get<int32_t>(0), 3);
    CHECK(std::memcmp(rbuf.data(), "hi!", 3) == 0);
    const uint8_t from[8] = {16, 2, 0xC1, 0x7A, 169, 254, 1, 2};
    CHECK(std::memcmp(sa, from, 8) == 0);
    const uint8_t bc[16] = {16, 2, 0xC1, 0x7A, 169, 254, 1, 255};
    const char all[] = "all";   // on the stack: buffer addresses must fit HIPC's 39 bits (rdata of the exe does not)
    CHECK_EQ(bsd.call(11, nxsvc::Args().i32(fd).i32(0).buf_a(all, 3).buf_a(bc, 16)).get<int32_t>(0), 3);
    char gb[8] = {};
    vsock::Addr gf;
    CHECK_EQ(gs.recvfrom(gfd, gb, sizeof gb, vsock::kMsgDontwait, &gf).ret, 3);
    CHECK_EQ(gf.ip, vnet::ip(169, 254, 1, 1));

    // Reject(Ipv4Address): the guest is out (LEFT rejected), the host sees the node leave; unknown address: NodeNotFound.
    CHECK_EQ(l.call(205, nxsvc::Args().u32(vnet::ip(169, 254, 1, 9))).result, kNodeNotFound);
    CHECK_EQ(l.call(205, nxsvc::Args().u32(j.vip)).result, 0u);
    CHECK_EQ(guest_lefts, 1);
    CHECK(guest_left == vnet::LeftReason::Rejected);
    CHECK(!guest->in_room());
    CHECK(signalled(ev));
    reset_signal(ev);
    CHECK_EQ(l.call(101, nxsvc::Args().buf_c(ni.data(), kNi).buf_c(up.data(), 64)).result, 0u);
    CHECK_EQ(ni[kLdn + 0x17], 1);
    CHECK_EQ(up[8], 2);                                                     // Disconnect
    CHECK_EQ(guest->join(rooms[0].room_id, kVersion, vnet::passphrase_hash(pass(), 64), &j), vnet::kLobbyRejected);

    // A second guest joins; DestroyNetwork closes the room under it.
    auto g2 = relay.client("Third");
    vnet::LeftReason g2_left = vnet::LeftReason::YouLeft;
    g2->set_events({nullptr, [&](vnet::LeftReason r) { g2_left = r; }});
    CHECK_EQ(g2->join(rooms[0].room_id, kVersion, vnet::passphrase_hash(pass(), 64), &j), 0);
    CHECK_EQ(j.vip, vnet::ip(169, 254, 1, 2));                             // the lowest free address again
    CHECK(vnet::current_link() != nullptr);
    CHECK_EQ(l.call(204).result, 0u);                                      // DestroyNetwork: 3 -> 2
    CHECK_EQ(state(l), 2u);
    CHECK(g2_left == vnet::LeftReason::HostClosed);
    CHECK(vnet::current_link() == nullptr);
    CHECK_EQ(l.call(3).get<uint16_t>(0), 3);                              // GetDisconnectReason: DestroyedByUser
    CHECK_EQ(l.call(1, nxsvc::Args().buf_c(ni1.data(), kNi)).result, kInvalidState);
    CHECK_EQ(l.call(201).result, 0u);                                      // CloseAccessPoint
    CHECK_EQ(l.call(401).result, 0u);                                      // Finalize
    CHECK_EQ(state(l), 0u);
    kernel::handle_close(ev);
}

// The process is the station: Scan finds a lobby host's room (NetworkInfo fields), Connect joins it; wrong version / passphrase fail;
// the host's Reject and its leaving disconnect the station with the reason set.
NXTEST(ldn_station_over_loopback) {
    nxsvc::Env env;
    vnet::LoopbackRelay relay;
    auto host = relay.client("Hosty");
    vnet::Joined hj;
    REQUIRE(host->create(room_config(), &hj) == 0);
    services::ldn_set_lobby(relay.client("Josh"));

    nxsvc::Session l = open_ldn();
    REQUIRE(l.valid());
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid().u64(0)).result, 0u);
    const uint32_t ev = l.call(100).copy.at(0);
    CHECK_EQ(l.call(102, nxsvc::Args().u16(0).pad(6)).result, kInvalidState);   // Scan needs AccessPoint or Station
    CHECK_EQ(l.call(300).result, 0u);                                      // OpenStation: 1 -> 4
    CHECK_EQ(state(l), 4u);

    // Scan(s16 channel, ScanFilter at 8: flag 0x5C = LocalCommunicationId) -> s16 count, NetworkInfo[] (0x22)
    std::vector<uint8_t> filter(0x60, 0);
    put<uint64_t>(filter, 0, kLcid);
    put<uint32_t>(filter, 0x5C, 1);
    std::vector<uint8_t> nis(24 * kNi);
    nxsvc::Reply sc = l.call(102, nxsvc::Args().u16(0).pad(6).bytes(filter.data(), 0x60).buf_b(nis.data(), nis.size()));
    CHECK_EQ(sc.result, 0u);
    REQUIRE(sc.get<uint16_t>(0) == 1);
    std::vector<uint8_t> ni(nis.begin(), nis.begin() + kNi);
    CHECK_EQ(at<uint64_t>(ni, 0), kLcid);
    CHECK_EQ(at<uint16_t>(ni, 0x0A), 0);
    CHECK_EQ(ni[kLdn + 0x17], 1);
    CHECK_EQ(at<uint32_t>(ni, kNodes), vnet::ip(169, 254, 1, 1));
    CHECK(std::strcmp(reinterpret_cast<const char*>(ni.data() + kNodes + 0xC), "Hosty") == 0);
    CHECK_EQ(at<int16_t>(ni, kNodes + 0x2E), kVersion);
    CHECK_EQ(at<uint16_t>(ni, kLdn + 0x21A), 3);
    put<uint64_t>(filter, 0, kLcid + 1);
    CHECK_EQ(l.call(102, nxsvc::Args().u16(0).pad(6).bytes(filter.data(), 0x60).buf_b(nis.data(), nis.size())).get<uint16_t>(0), 0);

    // Connect(SecurityConfig, UserConfig, s32 version, u32 option, NetworkInfo in 0x19): version and passphrase checked by the room.
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion - 1).data(), 0x7C).buf_x(ni.data(), kNi)).result, kVersionTooLow);
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion, 0xFF).data(), 0x7C).buf_x(ni.data(), kNi)).result, kConnectFailure);
    CHECK_EQ(state(l), 4u);
    reset_signal(ev);
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(ni.data(), kNi)).result, 0u);
    CHECK_EQ(state(l), 5u);
    CHECK(signalled(ev));
    reset_signal(ev);
    CHECK_EQ(l.call(2).get<uint32_t>(0), vnet::ip(169, 254, 1, 2));
    std::vector<uint8_t> cur(kNi);
    CHECK_EQ(l.call(1, nxsvc::Args().buf_c(cur.data(), kNi)).result, 0u);
    CHECK_EQ(cur[kLdn + 0x17], 2);
    CHECK(std::strcmp(reinterpret_cast<const char*>(cur.data() + kNodes + 0x40 + 0xC), "GuestUser") == 0);
    CHECK(vnet::current_link() != nullptr);
    CHECK_EQ(l.call(4).get<uint8_t>(0x10), static_cast<uint8_t>('p'));   // GetSecurityParameter: SessionId at 0x10

    // Disconnect: 5 -> 4, DisconnectedByUser; then again, and the host rejects us.
    CHECK_EQ(l.call(304).result, 0u);
    CHECK_EQ(state(l), 4u);
    CHECK_EQ(l.call(3).get<uint16_t>(0), 1);
    CHECK(vnet::current_link() == nullptr);
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(ni.data(), kNi)).result, 0u);
    reset_signal(ev);
    CHECK_EQ(host->reject(vnet::ip(169, 254, 1, 2)), 0);
    CHECK_EQ(state(l), 4u);
    CHECK(signalled(ev));
    CHECK_EQ(l.call(3).get<uint16_t>(0), 5);                              // Rejected
    CHECK(vnet::current_link() == nullptr);
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(ni.data(), kNi)).result, kConnectRejected);

    // The host leaves: the room is gone (Connect: not found); a new host's room, joined, then closed under us.
    host->leave();
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(ni.data(), kNi)).result, kConnectNotFound);
    auto host2 = relay.client("Other");
    REQUIRE(host2->create(room_config(), &hj) == 0);
    sc = l.call(102, nxsvc::Args().u16(0).pad(6).bytes(filter.data(), 0x60).buf_b(nis.data(), nis.size()));
    put<uint64_t>(filter, 0, kLcid);
    sc = l.call(102, nxsvc::Args().u16(0).pad(6).bytes(filter.data(), 0x60).buf_b(nis.data(), nis.size()));
    REQUIRE(sc.get<uint16_t>(0) == 1);
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(nis.data(), kNi)).result, 0u);
    reset_signal(ev);
    host2->leave();
    CHECK_EQ(state(l), 4u);
    CHECK(signalled(ev));
    CHECK_EQ(l.call(3).get<uint16_t>(0), 3);                              // DestroyedByUser
    CHECK_EQ(l.call(301).result, 0u);                                      // CloseStation
    CHECK_EQ(l.call(401).result, 0u);
    kernel::handle_close(ev);
}

// Connect returns only once the room state that lists this node is applied: with the state 300 ms behind JOINED, GetNetworkInfo right
// after Connect already has the host as node 0 (connected), this node and the advertise data. With no room state at all Connect fails
// after the 5 s wait and the node is out of the room again.
NXTEST(ldn_connect_waits_for_room_state) {
    nxsvc::Env env;
    vnet::LoopbackRelay relay;
    auto host = relay.client("Hosty");
    vnet::Joined hj;
    REQUIRE(host->create(room_config(), &hj) == 0);
    auto late = std::make_shared<LateRoomState>(relay.client("Josh"), 300);
    services::ldn_set_lobby(late);

    nxsvc::Session l = open_ldn();
    REQUIRE(l.valid());
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid().u64(0)).result, 0u);
    CHECK_EQ(l.call(300).result, 0u);                                      // OpenStation
    std::vector<uint8_t> filter(0x60, 0);
    put<uint64_t>(filter, 0, kLcid);
    put<uint32_t>(filter, 0x5C, 1);
    std::vector<uint8_t> nis(4 * kNi);
    REQUIRE(l.call(102, nxsvc::Args().u16(0).pad(6).bytes(filter.data(), 0x60).buf_b(nis.data(), nis.size())).get<uint16_t>(0) == 1);

    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(nis.data(), kNi)).result, 0u);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(waited >= 250);                                                   // Connect waited for the state
    CHECK_EQ(state(l), 5u);
    std::vector<uint8_t> ni(kNi);
    CHECK_EQ(l.call(1, nxsvc::Args().buf_c(ni.data(), kNi)).result, 0u);    // GetNetworkInfo, at once
    CHECK_EQ(ni[kLdn + 0x17], 2);
    CHECK_EQ(at<uint32_t>(ni, kNodes), vnet::ip(169, 254, 1, 1));           // node 0: the host
    CHECK_EQ(ni[kNodes + 0xA], 0);
    CHECK_EQ(ni[kNodes + 0xB], 1);                                          // isConnected
    CHECK(std::strcmp(reinterpret_cast<const char*>(ni.data() + kNodes + 0xC), "Hosty") == 0);
    CHECK_EQ(at<uint32_t>(ni, kNodes + 0x40), vnet::ip(169, 254, 1, 2));    // node 1: this node
    CHECK_EQ(ni[kNodes + 0x40 + 0xB], 1);
    CHECK(std::strcmp(reinterpret_cast<const char*>(ni.data() + kNodes + 0x40 + 0xC), "GuestUser") == 0);
    CHECK_EQ(at<uint16_t>(ni, kLdn + 0x21A), 3);                            // the advertise data
    CHECK(std::memcmp(ni.data() + kLdn + 0x21C, "adv", 3) == 0);
    CHECK_EQ(l.call(304).result, 0u);                                      // Disconnect
    CHECK(wait_until([&] { return host->room().nodes.size() == 1; }));

    // No room state ever: kConnectFailure after the wait, back in Station, the host alone again.
    late->set_delay(-1);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK_EQ(l.call(302, nxsvc::Args().bytes(connect_args(kVersion).data(), 0x7C).buf_x(nis.data(), kNi)).result, kConnectFailure);
    CHECK(std::chrono::steady_clock::now() - t1 >= std::chrono::milliseconds(4900));
    CHECK_EQ(state(l), 4u);
    CHECK(!late->in_room());
    CHECK_EQ(host->room().nodes.size(), 1u);
    CHECK(vnet::current_link() == nullptr);
    CHECK_EQ(l.call(401).result, 0u);
}

// Two relay clients through a real server (launcher --host): create, scan, join, unicast + broadcast, reject, leave, host close.
NXTEST(ldn_live_relay) {
    const char* addr = std::getenv("NXRT_TEST_RELAY");
    if (!addr || !*addr) {
        nxtest::skip("NXRT_TEST_RELAY not set (host:port of a relay server, e.g. PokopiaLauncher --host --port 39670)");
        return;
    }
    vnet::RelayOptions o;
    REQUIRE(vnet::parse_server(addr, &o.host, &o.port));
    o.nickname = "LiveHost";
    auto a = vnet::make_relay_client(o);
    o.nickname = "LiveGuest";
    auto b = vnet::make_relay_client(o);
    std::printf("  live: relay %s\n", addr);
    REQUIRE(a->connect() == 0);
    REQUIRE(b->connect() == 0);

    std::atomic<int> a_states{0};
    a->set_events({[&](const vnet::Room&) { ++a_states; }, nullptr});
    std::atomic<int> b_left{-1};
    b->set_events({nullptr, [&](vnet::LeftReason r) { b_left = static_cast<int>(r); }});

    vnet::RoomConfig cfg = room_config();
    cfg.local_comm_id = kLcid ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());   // our own room only
    vnet::Joined ja, jb;
    REQUIRE(a->create(cfg, &ja) == 0);
    std::printf("  live: created room %u, host %s\n", ja.room_id, vnet::ip_str(ja.vip).c_str());
    CHECK_EQ(ja.vip & 0xFF, 1u);
    std::vector<vnet::Room> rooms;
    REQUIRE(b->scan(cfg.local_comm_id, 0xFFFF, &rooms) == 0);
    REQUIRE(rooms.size() == 1);
    CHECK(rooms[0].nodes.at(0).nickname == "LiveHost");
    CHECK(rooms[0].adv == cfg.adv);
    CHECK_EQ(b->join(rooms[0].room_id, kVersion - 1, cfg.pass_hash, &jb), vnet::kLobbyVersion);
    std::array<uint8_t, 16> bad = cfg.pass_hash;
    bad[0] ^= 1;
    CHECK_EQ(b->join(rooms[0].room_id, kVersion, bad, &jb), vnet::kLobbyPassword);
    REQUIRE(b->join(rooms[0].room_id, kVersion, cfg.pass_hash, &jb) == 0);
    std::printf("  live: guest joined as %s (node %u)\n", vnet::ip_str(jb.vip).c_str(), jb.node_id);
    CHECK_EQ(jb.vip, ja.vip + 1);
    CHECK(wait_until([&] { return a->room().nodes.size() == 2; }));
    CHECK(wait_until([&] { return b->room().nodes.size() == 2; }));

    vsock::Stack sa(a->link()), sb(b->link());
    const int fa = sa.socket(2, 2, 0).ret, fb = sb.socket(2, 2, 0).ret;
    REQUIRE(sa.bind(fa, {0, kPort}).ret == 0);
    REQUIRE(sb.bind(fb, {0, kPort}).ret == 0);
    const vsock::Addr to_b{jb.vip, kPort};
    CHECK_EQ(sa.sendto(fa, "unicast", 7, 0, &to_b).ret, 7);
    char buf[64] = {};
    vsock::Addr from;
    int got = -1;
    wait_until([&] { return (got = sb.recvfrom(fb, buf, sizeof buf, vsock::kMsgDontwait, &from).ret) > 0; });
    CHECK_EQ(got, 7);
    CHECK(std::memcmp(buf, "unicast", 7) == 0);
    CHECK_EQ(from.ip, ja.vip);
    int32_t on = 1;
    sb.setsockopt(fb, vsock::kSolSocket, vsock::kSoBroadcast, &on, 4);
    const vsock::Addr bc{ja.vip | 0xFF, kPort};
    CHECK_EQ(sb.sendto(fb, "broadcast", 9, 0, &bc).ret, 9);
    got = -1;
    wait_until([&] { return (got = sa.recvfrom(fa, buf, sizeof buf, vsock::kMsgDontwait, &from).ret) > 0; });
    CHECK_EQ(got, 9);
    CHECK_EQ(from.ip, jb.vip);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(sb.recvfrom(fb, buf, sizeof buf, vsock::kMsgDontwait, &from).err, vsock::kEagain);   // no self echo
    std::printf("  live: unicast host->guest and broadcast guest->host delivered, no echo\n");

    // Reject, then the rejected client cannot rejoin; a guest leaving; the host closing.
    CHECK_EQ(a->reject(jb.vip), 0);
    CHECK(wait_until([&] { return b_left.load() == static_cast<int>(vnet::LeftReason::Rejected); }));
    CHECK(!b->in_room());
    CHECK(wait_until([&] { return a->room().nodes.size() == 1; }));
    CHECK_EQ(b->join(ja.room_id, kVersion, cfg.pass_hash, &jb), vnet::kLobbyRejected);
    auto c = vnet::make_relay_client(o);
    std::atomic<int> c_left{-1};
    c->set_events({nullptr, [&](vnet::LeftReason r) { c_left = static_cast<int>(r); }});
    REQUIRE(c->join(ja.room_id, kVersion, cfg.pass_hash, &jb) == 0);
    CHECK(wait_until([&] { return a->room().nodes.size() == 2; }));
    c->leave();
    CHECK(wait_until([&] { return a->room().nodes.size() == 1; }));
    REQUIRE(c->join(ja.room_id, kVersion, cfg.pass_hash, &jb) == 0);
    a->leave();
    CHECK(wait_until([&] { return c_left.load() == static_cast<int>(vnet::LeftReason::HostClosed); }));
    CHECK(wait_until([&] {
        std::vector<vnet::Room> r;
        return c->scan(cfg.local_comm_id, 0xFFFF, &r) == 0 && r.empty();
    }));
    std::printf("  live: reject, rejoin refused, leave, host close: ok (%d room states seen by the host)\n", a_states.load());
}

// The ldn service itself on a RelayClient (events and datagrams arrive on the reader thread): the process hosts through IPC, a second
// relay client joins, sends to the game's socket, leaves.
NXTEST(ldn_live_relay_ipc) {
    const char* addr = std::getenv("NXRT_TEST_RELAY");
    if (!addr || !*addr) {
        nxtest::skip("NXRT_TEST_RELAY not set");
        return;
    }
    nxsvc::Env env;
    vnet::RelayOptions o;
    REQUIRE(vnet::parse_server(addr, &o.host, &o.port));
    o.nickname = "IpcHost";
    services::ldn_set_lobby(vnet::make_relay_client(o));
    o.nickname = "IpcGuest";
    auto b = vnet::make_relay_client(o);

    nxsvc::Session l = open_ldn();
    REQUIRE(l.valid());
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid().u64(0)).result, 0u);
    const uint32_t ev = l.call(100).copy.at(0);
    CHECK_EQ(l.call(200).result, 0u);
    std::vector<uint8_t> args = create_args(4);
    const uint64_t lcid = kLcid ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    put<uint64_t>(args, 0x78, lcid);
    REQUIRE(l.call(202, nxsvc::Args().bytes(args.data(), args.size())).result == 0);
    const uint32_t host_ip = l.call(2).get<uint32_t>(0);
    std::printf("  live ipc: CreateNetwork -> %s\n", vnet::ip_str(host_ip).c_str());
    reset_signal(ev);

    std::vector<vnet::Room> rooms;
    REQUIRE(b->scan(lcid, 0xFFFF, &rooms) == 0);
    REQUIRE(rooms.size() == 1);
    vnet::Joined j;
    REQUIRE(b->join(rooms[0].room_id, kVersion, vnet::passphrase_hash(pass(), 64), &j) == 0);
    CHECK(wait_until([&] { return signalled(ev); }));   // the ROOM_STATE push, from the reader thread
    std::vector<uint8_t> ni(kNi);
    CHECK(wait_until([&] { return l.call(1, nxsvc::Args().buf_c(ni.data(), kNi)).result == 0 && ni[kLdn + 0x17] == 2; }));
    CHECK(std::strcmp(reinterpret_cast<const char*>(ni.data() + kNodes + 0x40 + 0xC), "IpcGuest") == 0);

    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    const int32_t fd = game_socket(bsd);
    vsock::Stack gs(b->link());
    const int gfd = gs.socket(2, 2, 0).ret;
    REQUIRE(gs.bind(gfd, {0, kPort}).ret == 0);
    const vsock::Addr to{host_ip, kPort};
    CHECK_EQ(gs.sendto(gfd, "relay!", 6, 0, &to).ret, 6);
    std::vector<uint8_t> rbuf(0x1000);
    uint8_t sa[16] = {};
    int32_t got = -1;
    wait_until([&] {
        got = bsd.call(9, nxsvc::Args().i32(fd).i32(0x80).buf_b(rbuf.data(), 0x1000).buf_b(sa, 16)).get<int32_t>(0);
        return got > 0;
    });
    CHECK_EQ(got, 6);
    CHECK(std::memcmp(rbuf.data(), "relay!", 6) == 0);
    CHECK_EQ(sa[7], static_cast<uint8_t>(j.vip));
    std::printf("  live ipc: guest %s -> game socket: %d bytes\n", vnet::ip_str(j.vip).c_str(), got);

    reset_signal(ev);
    b->leave();
    CHECK(wait_until([&] { return signalled(ev); }));
    std::vector<uint8_t> up(64);
    CHECK(wait_until([&] {
        l.call(101, nxsvc::Args().buf_c(ni.data(), kNi).buf_c(up.data(), 64));
        return ni[kLdn + 0x17] == 1;
    }));
    CHECK_EQ(l.call(204).result, 0u);
    CHECK_EQ(l.call(401).result, 0u);
    std::printf("  live ipc: guest left, network destroyed\n");
    kernel::handle_close(ev);
}
