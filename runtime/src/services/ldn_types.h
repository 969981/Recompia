// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u types shared by the Link Play (ldn_linkplay.cpp) and native (ldn_native.cpp) backends: result codes, enums, the SDK struct layouts
// with their size checks, and the raw-argument offsets of the CMIF commands. Provenance: the header of ldn_linkplay.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/ipc.h"
#include "nxrt/services.h"

namespace nxrt::services::ldn_impl {

// ---- result codes (Ryujinx Ldn/ResultCode.cs; module 203) -----------------------------------------------------------------------------
constexpr uint32_t kDeviceNotAvailable = res(203, 16);
constexpr uint32_t kInvalidState = res(203, 32);
constexpr uint32_t kNodeNotFound = res(203, 48);
constexpr uint32_t kConnectFailure = res(203, 64);
constexpr uint32_t kConnectNotFound = res(203, 65);
constexpr uint32_t kConnectTimeout = res(203, 66);
constexpr uint32_t kConnectRejected = res(203, 67);
constexpr uint32_t kInvalidArgument = res(203, 96);
constexpr uint32_t kVersionTooLow = res(203, 113);
constexpr uint32_t kVersionTooHigh = res(203, 114);
constexpr uint32_t kTooManyPlayers = res(203, 144);

// ---- enums ----------------------------------------------------------------------------------------------------------------------------
enum State : uint32_t { kNone = 0, kInitialized = 1, kAccessPoint = 2, kAccessPointCreated = 3, kStation = 4, kStationConnected = 5, kError = 6 };
enum Reason : int16_t {
    kReasonNone = 0, kDisconnectedByUser = 1, kDisconnectedBySystem = 2, kDestroyedByUser = 3, kDestroyedBySystem = 4, kRejected = 5,
    kSignalLost = 6,
};
enum : uint8_t { kNodeConnect = 1, kNodeDisconnect = 2 };   // NodeLatestUpdate.StateChange
enum : uint32_t {                                            // ScanFilterFlag
    kFilterLocalCommunicationId = 1 << 0, kFilterSessionId = 1 << 1, kFilterNetworkType = 1 << 2, kFilterBssid = 1 << 3,
    kFilterSsid = 1 << 4, kFilterSceneId = 1 << 5,
};

// ---- layouts (Switchbrew "LDN services", Ryujinx Ldn/Types) ---------------------------------------------------------------------------
struct IntentId {
    uint64_t local_comm_id;
    uint16_t reserved1;
    uint16_t scene_id;
    uint32_t reserved2;
};
struct NetworkId {
    IntentId intent;
    uint8_t session_id[16];
};
struct Ssid {
    uint8_t length;
    char raw[33];
};
struct CommonNetworkInfo {
    uint8_t bssid[6];
    Ssid ssid;
    int16_t channel;
    int8_t link_level;
    uint8_t network_type;
    uint32_t reserved;
};
struct NodeInfo {
    uint32_t ipv4;
    uint8_t mac[6];
    int8_t node_id;
    uint8_t is_connected;
    char user_name[33];
    uint8_t reserved1;   // Platform from 19.0.0 (0 = NX)
    int16_t local_comm_version;
    uint8_t reserved2[16];
};
struct LdnNetworkInfo {
    uint8_t security_parameter[16];
    uint16_t security_mode;
    uint8_t station_accept_policy;
    uint8_t unknown1;
    uint8_t reserved1[2];
    uint8_t node_count_max;
    uint8_t node_count;
    NodeInfo nodes[8];
    uint8_t reserved2[2];
    uint16_t advertise_data_size;
    uint8_t advertise_data[384];
    uint8_t reserved3[140];
    uint64_t authentication_id;
};
struct NetworkInfo {
    NetworkId id;
    CommonNetworkInfo common;
    LdnNetworkInfo ldn;
};
struct NodeLatestUpdate {
    uint8_t state_change;
    uint8_t reserved[7];
};
struct ScanFilter {
    NetworkId id;
    uint32_t network_type;
    uint8_t bssid[6];
    Ssid ssid;
    uint8_t reserved[16];
    uint32_t flag;
};
struct NetworkConfig {
    IntentId intent;
    uint16_t channel;
    uint8_t node_count_max;
    uint8_t reserved1;
    uint16_t local_comm_version;
    uint8_t reserved2[10];
};
struct SecurityConfig {
    uint16_t mode;
    uint16_t passphrase_size;
    uint8_t passphrase[64];
};
struct SecurityParameter {
    uint8_t data[16];
    uint8_t session_id[16];
};
struct UserConfig {
    char user_name[33];
    uint8_t reserved[15];
};

static_assert(sizeof(IntentId) == 0x10 && sizeof(NetworkId) == 0x20 && sizeof(Ssid) == 0x22);
static_assert(sizeof(CommonNetworkInfo) == 0x30 && offsetof(CommonNetworkInfo, channel) == 0x28);
static_assert(sizeof(NodeInfo) == 0x40 && offsetof(NodeInfo, user_name) == 0x0C && offsetof(NodeInfo, local_comm_version) == 0x2E);
static_assert(sizeof(LdnNetworkInfo) == 0x430 && offsetof(LdnNetworkInfo, nodes) == 0x18 && offsetof(LdnNetworkInfo, advertise_data_size) == 0x21A &&
              offsetof(LdnNetworkInfo, advertise_data) == 0x21C && offsetof(LdnNetworkInfo, authentication_id) == 0x428);
static_assert(sizeof(NetworkInfo) == 0x480 && offsetof(NetworkInfo, ldn) == 0x50);
static_assert(sizeof(NodeLatestUpdate) == 8);
static_assert(sizeof(ScanFilter) == 0x60 && offsetof(ScanFilter, ssid) == 0x2A && offsetof(ScanFilter, flag) == 0x5C);
static_assert(sizeof(NetworkConfig) == 0x20 && offsetof(NetworkConfig, local_comm_version) == 0x14);
static_assert(sizeof(SecurityConfig) == 0x44 && sizeof(SecurityParameter) == 0x20 && sizeof(UserConfig) == 0x30);

// Raw argument offsets (CMIF, natural alignment: NetworkConfig is 8-aligned).
constexpr size_t kCreateUser = 0x44, kCreateNetCfg = 0x78;                                 // CreateNetwork
constexpr size_t kCreatePrivUser = 0x64, kCreatePrivNetCfg = 0x98;   // CreateNetworkPrivate (SecurityParameter at 0x44: unused)
constexpr size_t kConnectUser = 0x44, kConnectVersion = 0x74;                              // Connect
constexpr size_t kConnPrivSecParam = 0x44, kConnPrivUser = 0x64, kConnPrivVersion = 0x94, kConnPrivNetCfg = 0xA0;   // ConnectPrivate
constexpr uint32_t kSubnetMask = 0xFFFFFF00;   // the game asks MakeSubnetMask(24) too
// The first 4 bytes of a Link Play SessionId (ldn_linkplay.h SessionRef): a protocol tag from the relay's first version, the same for every
// game and fixed (older builds read it to map a scanned network back to its room).
constexpr uint8_t kSessionTag[4] = {'p', 'k', 'p', 'R'};
// Connect waits this long for the room state that lists the new node (the relay sends ROOM_STATE right behind JOINED); the lobby's own
// request timeout is 5 s too (vnet::RelayOptions::timeout_ms).
constexpr int kRoomStateWaitMs = 5000;
// Host: datagrams from a node the game has not yet seen in a NetworkInfo are held (vnet::HoldLink) until the game reads a table listing
// it, at most this long (pokopia-native windows/src/coop/coop_mpnet.c ACCEPT_WAIT_MS: the game polls GetNetworkInfo every frame), and at
// most kHoldCap of them. A game's session layer may drop a datagram from a sender outside the member table it last read (KTGL, Pokopia's
// engine, does, and the new station's first one is its join request); for any other game a new node's first datagrams wait at most
// kHoldMs, less once the game reads the table.
constexpr int kHoldMs = 300;
constexpr size_t kHoldCap = 32;

template <class T>
T arg_at(const ipc::Request& req, size_t off) {
    return req.arg<T>(off);
}

}  // namespace nxrt::services::ldn_impl
