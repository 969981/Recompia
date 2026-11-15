// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u Link Play backend, auto mode: the process lobby, the relay in this process and its clients, LAN discovery of other hosts and the
// parallel scan of the relays found (see the header of ldn_linkplay.cpp, "[net] server = auto").
#include "ldn_internal.h"


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
#include "nxrt/text.h"
#include "../net/vnet.h"
#include "services_internal.h"

namespace nxrt::services::ldn_impl {

// Everything of a relay client but its host and port. False: [net] room_code is not a code (local wireless stays off).
bool relay_options(vnet::RelayOptions* o) {
    o->nickname = nickname();
    o->password = config_get_str("net", "password", "");
    if (!room_code_setup(&o->room_key)) return false;
    const std::string udp = text::lower(config_get_str("net", "udp", "auto"));
    o->udp = !(udp == "off" || udp == "0" || udp == "false" || udp == "no" || udp == "tcp");
    if (o->udp && udp != "auto" && udp != "on" && udp != "1" && udp != "true" && udp != "yes")
        NXLOG_W("net", "ldn: [net] udp = \"%s\" is not auto or off: auto", udp.c_str());
    return true;
}

// [net] server: empty or off = local wireless off; auto (also when the key is absent) = auto mode, no lobby until a relay is picked;
// host:port = a relay client to it. *auto_mode: auto mode.
std::shared_ptr<vnet::Lobby> process_lobby(bool* auto_mode) {
    LobbySlot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.built) {
        s.built = true;
        const std::string server = config_get_str("net", "server", "auto");
        std::string v = text::lower(server);
        text::trim_blanks(v);
        vnet::RelayOptions o;
        if (v.empty() || v == "off") {
            NXLOG_I("net", "ldn: [net] server is %s: local wireless off (scans find nothing)", v.empty() ? "empty" : "off");
        } else if (v == "auto") {
            if (relay_options(&o)) {
                s.auto_mode = true;
                s.base = o;
                NXLOG_I("net", "ldn: [net] server = auto: hosting starts a relay in this process (port %u), visiting looks for hosts on the "
                        "local network", vnet::kRelayDefaultPort);
            }
        } else if (!vnet::parse_server(server, &o.host, &o.port)) {
            NXLOG_W("net", "ldn: [net] server = \"%s\" is not host:port, auto or off: local wireless off", server.c_str());
        } else if (relay_options(&o)) {
            s.lobby = vnet::make_relay_client(o);
        }
    }
    if (auto_mode) *auto_mode = s.auto_mode;
    return s.lobby;
}

// Auto mode: the relay client for host:port (made on first use, then kept; caller holds s.mu).
std::shared_ptr<vnet::Lobby> relay_client_locked(LobbySlot& s, const std::string& host, uint16_t port) {
    AutoRelay& r = s.relays[host + ":" + std::to_string(port)];
    if (!r.client) {
        vnet::RelayOptions o = s.base;
        o.host = host;
        o.port = port;
        r.client = vnet::make_relay_client(o);
        r.seen = std::chrono::steady_clock::now();
    }
    return r.client;
}

// Auto mode: makes the client of the relay at host:port the process lobby (the one rooms go through: hosting, a Connect) and returns it;
// the previous one stays in slot().relays as a scan client. Outside auto mode: the process lobby as it is.
std::shared_ptr<vnet::Lobby> auto_relay(const std::string& host, uint16_t port) {
    LobbySlot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.auto_mode) return s.lobby;
    std::shared_ptr<vnet::Lobby> c = relay_client_locked(s, host, port);
    if (s.lobby != c) {
        s.lobby = c;
        s.target = host + ":" + std::to_string(port);
        NXLOG_I("net", "ldn: auto: rooms go through the relay at %s", s.target.c_str());
    }
    return c;
}

// A relay's identity in the session id (bytes 8..13): the first 6 bytes of SHA-256 of its WELCOME server version, which carries a random
// instance id on the embedded relay. The host (at 127.0.0.1) and every visitor (at a LAN address) get the same value
// for one relay, and two relays get different ones. 0: not known (never connected).
uint64_t relay_tag(const vnet::Lobby& l) {
    const std::string v = l.server_version();
    if (v.empty()) return 0;
    const auto h = vnet::sha256(v.data(), v.size());
    uint64_t t = 0;
    for (int i = 0; i < 6; ++i) t |= uint64_t{h[static_cast<size_t>(i)]} << (8 * i);
    return t ? t : 1;
}

void firewall_hint() {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        NXLOG_I("net", "ldn: if Windows Firewall asks about this program, allow it on private networks (Link Play uses TCP and UDP port %u)",
                vnet::kRelayDefaultPort);
}

// Auto mode, CreateNetwork: the relay in this process on 0.0.0.0:39660, started on the first call that can start it and never stopped
// (it runs until the process exits). A port already taken (another game on this PC) is that relay's: it is used as it
// is. Either way the host is a relay client to 127.0.0.1:39660.
std::shared_ptr<vnet::Lobby> host_relay() {
    static std::mutex mu;
    static vnet::RelayServer* server = nullptr;   // deliberately never deleted
    static std::string last;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!server) {
            vnet::RelayServerOptions o;
            o.port = vnet::kRelayDefaultPort;
            o.password = config_get_str("net", "password", "");
            o.nickname = nickname();
            // the random instance id makes relay_tag() unique per relay
            o.server_version = std::string("nxrt relay (") + vnet::build_stamp() + ") #" + vnet::room_code_new();
            std::string why;
            bool in_use = false;
            if (auto srv = vnet::RelayServer::start(o, &why, &in_use)) {
                server = srv.release();
                last.clear();
                for (const vnet::LocalAddr& a : vnet::local_ipv4())
                    if (!vnet::is_link_local(a.ip))
                        NXLOG_I("net", "ldn: hosting: visitors with [net] server set reach this PC at %s:%u (%s)", vnet::ip_str(a.ip).c_str(),
                                server->port(), a.adapter.c_str());
                firewall_hint();
            } else if (why != last) {
                last = why;
                if (in_use) NXLOG_I("net", "ldn: hosting: %s on this PC (the launcher's relay or another game): hosting on that relay", why.c_str());
                else NXLOG_W("net", "ldn: hosting: cannot start the relay: %s", why.c_str());
            }
        }
    }
    return auto_relay("127.0.0.1", vnet::kRelayDefaultPort);
}

// Auto mode, Scan: the last LAN discovery, a new one at most every kDiscoveryEveryMs (waiting kDiscoveryWaitMs for answers). The relays
// found are logged when the result changes, not per discovery.
constexpr int kDiscoveryWaitMs = 400, kDiscoveryEveryMs = 2000;

std::vector<vnet::FoundRelay> discover_cached() {
    Discovery& d = discovery();
    std::lock_guard<std::mutex> lk(d.mu);
    const auto now = std::chrono::steady_clock::now();
    if (!d.ran || now - d.at >= std::chrono::milliseconds(kDiscoveryEveryMs)) {
        d.found = vnet::discover_relays(vnet::kRelayDefaultPort, kDiscoveryWaitMs);
        d.at = std::chrono::steady_clock::now();
        d.ran = true;
        std::string seen = "-";
        for (const vnet::FoundRelay& f : d.found) seen += vnet::ip_str(f.ip) + ":" + std::to_string(f.port) + "/" + std::to_string(f.open_rooms) + " ";
        if (seen != d.logged) {
            d.logged = seen;
            if (d.found.empty()) {
                NXLOG_I("net", "ldn: no Link Play host found on the local network (the host picks Invite Others to Visit first; a firewall on "
                        "either PC can block UDP/TCP port %u)", vnet::kRelayDefaultPort);
                firewall_hint();
            }
            for (const vnet::FoundRelay& f : d.found)
                NXLOG_I("net", "ldn: Link Play host %s:%u (%s, %u open room%s)", vnet::ip_str(f.ip).c_str(), f.port, f.nickname.c_str(),
                        f.open_rooms, f.open_rooms == 1 ? "" : "s");
        }
    }
    return d.found;
}

// Auto mode, Scan: the rooms of every relay found, asked in parallel. A relay reporting no open room is skipped unless this player has a
// room code (open rooms count keyless rooms only). Each relay's SCAN runs on a thread of its own; the scan waits for all of them at most
// kScanAllMs, a relay that has not answered by then is left out of this scan (its thread finishes on its own and is not started again for
// that relay while it runs). Clients of relays no discovery listed for kRelayForgetMs are dropped, never the process lobby (the one a room
// goes through).
constexpr int kScanAllMs = 1500, kRelayForgetMs = 10000;

std::vector<RelayRooms> scan_relays(const std::vector<vnet::FoundRelay>& found, uint64_t lcid, uint16_t scene) {
    struct Job {
        RelayRooms rr;
        std::shared_ptr<vnet::Lobby> client;
        std::shared_ptr<std::atomic<bool>> busy;
    };
    std::vector<Job> jobs;
    {
        LobbySlot& s = slot();
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.auto_mode) return {};
        const bool keyed = s.base.room_key != std::array<uint8_t, 16>{};
        const auto now = std::chrono::steady_clock::now();
        for (const vnet::FoundRelay& f : found) {
            const std::string host = vnet::ip_str(f.ip);
            std::shared_ptr<vnet::Lobby> c = relay_client_locked(s, host, f.port);
            AutoRelay& r = s.relays[host + ":" + std::to_string(f.port)];
            r.seen = now;
            if (!keyed && f.open_rooms == 0) continue;
            Job j;
            j.rr.host = host;
            j.rr.port = f.port;
            j.rr.loopback = (f.ip >> 24) == 127;
            j.client = c;
            j.busy = r.busy;
            jobs.push_back(std::move(j));
        }
        for (auto it = s.relays.begin(); it != s.relays.end();) {
            if (it->second.client != s.lobby && now - it->second.seen >= std::chrono::milliseconds(kRelayForgetMs)) it = s.relays.erase(it);
            else ++it;
        }
    }
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        size_t pending = 0;
        std::vector<RelayRooms> got;
    };
    auto sh = std::make_shared<Shared>();
    for (Job& j : jobs) {
        if (j.busy->exchange(true)) continue;   // its scan from an earlier call is still waiting
        {
            std::lock_guard<std::mutex> lk(sh->mu);
            ++sh->pending;
        }
        std::thread([sh, j, lcid, scene]() mutable {
            const int e = j.client->scan(lcid, scene, &j.rr.rooms);
            j.rr.tag = relay_tag(*j.client);
            j.busy->store(false);
            if (e) NXLOG_D("net", "ldn: scan of the relay at %s:%u failed: %s", j.rr.host.c_str(), j.rr.port, vnet::lobby_err_str(e));
            std::lock_guard<std::mutex> lk(sh->mu);
            if (!e) sh->got.push_back(std::move(j.rr));
            --sh->pending;
            sh->cv.notify_all();
        }).detach();
    }
    std::unique_lock<std::mutex> lk(sh->mu);
    sh->cv.wait_for(lk, std::chrono::milliseconds(kScanAllMs), [&] { return sh->pending == 0; });
    std::vector<RelayRooms> out = std::move(sh->got);
    sh->got.clear();   // a late answer lands here and is dropped with sh
    return out;
}

}  // namespace nxrt::services::ldn_impl
