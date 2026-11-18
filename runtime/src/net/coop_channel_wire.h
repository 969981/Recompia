// SPDX-License-Identifier: GPL-3.0-or-later
// The co-op channel's frame layout (described in nxrt/coop_channel.h): header and body offsets, sizes and the little-endian helpers the
// stream (coop_stream.cpp) and the pump (coop_channel.cpp) both use.
#pragma once
#include <cstddef>
#include <cstdint>

#include "vnet.h"

namespace nxrt::vnet::coop_wire {

constexpr uint8_t kMagic[4] = {'N', 'X', 'C', 'C'};
constexpr uint8_t kVersion = 1;
enum Kind : uint8_t { kHello = 1, kData = 2, kAck = 3, kBye = 4 };

// Header: magic (4), version (1), kind (1), zero (2), sender's connection id (4), receiver's connection id (4).
constexpr size_t kHeader = 16;
// Acknowledgement block, in DATA and as the ACK body: ack (4) = the next seq expected (everything below arrived), sack (4) = bit i set:
// seq ack + 1 + i arrived early and is kept.
constexpr size_t kAckBlock = 8;
// DATA body: seq (4), acknowledgement block (8), flags (1); a first fragment then type (4) and total size (8).
constexpr size_t kDataAck = 4;
constexpr size_t kDataFlags = kDataAck + kAckBlock;
constexpr size_t kDataHead = kDataFlags + 1;
constexpr size_t kFirstHead = 12;
constexpr uint8_t kFlagFirst = 0x01;
// Fragment bytes per DATA frame: every fragment the same size, so a first fragment also fits one datagram.
constexpr size_t kFragmentMax = kMaxDatagram - kHeader - kDataHead - kFirstHead;
// Frames in flight per stream (sender) and kept early (receiver).
constexpr uint32_t kWindow = 32;

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint32_t get32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
inline uint64_t get64(const uint8_t* p) { return uint64_t{get32(p)} | (uint64_t{get32(p + 4)} << 32); }

struct Header {
    Kind kind = kHello;
    uint32_t conn = 0;        // the sender's connection id
    uint32_t peer_conn = 0;   // the receiver's, as the sender knows it (0: not yet)
};

inline void put_header(uint8_t* p, const Header& h) {
    for (int i = 0; i < 4; ++i) p[i] = kMagic[i];
    p[4] = kVersion;
    p[5] = h.kind;
    put16(p + 6, 0);
    put32(p + 8, h.conn);
    put32(p + 12, h.peer_conn);
}

// False when p is not a channel frame of this version.
inline bool get_header(const uint8_t* p, size_t n, Header* h) {
    if (n < kHeader) return false;
    for (int i = 0; i < 4; ++i)
        if (p[i] != kMagic[i]) return false;
    if (p[4] != kVersion || p[5] < kHello || p[5] > kBye) return false;
    h->kind = static_cast<Kind>(p[5]);
    h->conn = get32(p + 8);
    h->peer_conn = get32(p + 12);
    return h->conn != 0;
}

}  // namespace nxrt::vnet::coop_wire
