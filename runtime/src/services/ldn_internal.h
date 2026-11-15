// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the ldn:u backends: helpers shared by ldn.cpp, ldn_common.cpp, ldn_auto_relay.cpp, ldn_linkplay.cpp and ldn_native.cpp.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/services_api.h"
#include "../net/lobby.h"
#include "../net/relay_server.h"
#include "../net/vnet.h"
#include "ldn_types.h"

namespace nxrt::services::ldn_impl {

// ---- ldn_common.cpp ----------------------------------------------------------------------------------------------------
std::string nickname();

bool room_code_setup(std::array<uint8_t, 16>* key);
void copy_name(char (&dst)[33], const std::string& s);
std::string hex_head(const std::vector<uint8_t>& b);
void note(uint32_t id, const char* name);
extern std::mutex g_seen_mu;      // note()'s set of commands logged so far; ldn_reset() clears it
extern std::set<uint32_t> g_seen;

// ---- session variant (services_api.h ldnapi) ----------------------------------------------------------------------------------------
struct Variant {
    std::mutex mu;
    uint8_t value = 0;
    std::string what;
    std::vector<std::string> mods;   // ldnapi::set_coop_mods
};
Variant& variant();
uint8_t own_variant(std::string* what = nullptr, std::vector<std::string>* mods = nullptr);
int16_t wire_version(int16_t game);
int16_t game_version(int16_t wire);
uint8_t variant_of(int16_t wire);

// ---- ldn_auto_relay.cpp ------------------------------------------------------------------------------------------------
struct AutoRelay {                // auto mode: one client per relay address, kept across scans
    std::shared_ptr<vnet::Lobby> client;
    std::chrono::steady_clock::time_point seen{};   // the last discovery that listed it
    std::shared_ptr<std::atomic<bool>> busy = std::make_shared<std::atomic<bool>>(false);   // a scan of it is in flight
};
struct LobbySlot {
    std::mutex mu;
    bool built = false;
    bool auto_mode = false;      // [net] server = auto: lobby follows the relay in use (auto_relay)
    vnet::RelayOptions base;     // auto: the options of every relay client (host and port per relay)
    std::string target;          // auto: host:port of lobby
    std::map<std::string, AutoRelay> relays;   // auto: host:port -> its client (lobby is one of them)
    std::shared_ptr<vnet::Lobby> lobby;
};
LobbySlot& slot();             // the linkplay backend's (ldn_linkplay.cpp; its reset() clears it)
std::shared_ptr<vnet::Lobby> process_lobby(bool* auto_mode);
std::shared_ptr<vnet::Lobby> auto_relay(const std::string& host, uint16_t port);
uint64_t relay_tag(const vnet::Lobby& l);
std::shared_ptr<vnet::Lobby> host_relay();

struct Discovery {
    std::mutex mu;
    bool ran = false;
    std::chrono::steady_clock::time_point at{};
    std::vector<vnet::FoundRelay> found;
    std::string logged;
};
Discovery& discovery();        // the linkplay backend's (ldn_linkplay.cpp; its reset() clears it)
std::vector<vnet::FoundRelay> discover_cached();

struct RelayRooms {
    std::string host;
    uint16_t port = 0;
    bool loopback = false;
    uint64_t tag = 0;
    std::vector<vnet::Room> rooms;
};
std::vector<RelayRooms> scan_relays(const std::vector<vnet::FoundRelay>& found, uint64_t lcid, uint16_t scene);

}  // namespace nxrt::services::ldn_impl
