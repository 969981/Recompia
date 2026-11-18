// SPDX-License-Identifier: GPL-3.0-or-later
// One host-side UDP port on the current session's virtual network, for runtime code that talks to the other processes of a Link Play
// session (the co-op channel, net/coop_channel.cpp). Each ldn backend makes its own (ldn_backend.h open_port), on the sockets the game's
// datagrams use, so the port reaches every member whatever carries the session (one PC, LAN, relay). Datagrams are unreliable and at
// most vnet::kMaxDatagram bytes. Used by one thread at a time.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace nxrt::vnet {

class SessionPort {
public:
    virtual ~SessionPort() = default;
    // This process's virtual address in the session the port was opened in; 0 once it has left that session.
    virtual uint32_t local_ip() const = 0;
    // The address of the process that opened the session (the network's host).
    virtual uint32_t host_ip() const = 0;
    // Sends one datagram to dst_ip (a member, or the subnet's .255 for every other member) on the port's own port number.
    // False when it could not be sent (dropped on the way is not reported).
    virtual bool send(uint32_t dst_ip, const void* p, size_t n) = 0;
    // Waits up to timeout_ms (0: does not wait) for one datagram sent to the port. > 0: its size (cut at cap) and *src_ip;
    // 0: none; < 0: the port is dead (closed under it), open a new one.
    virtual int recv(void* p, size_t cap, uint32_t* src_ip, int timeout_ms) = 0;
};

// Opens port on the session this process is in now; null when it is in none (or the port cannot be bound).
using PortOpener = std::function<std::unique_ptr<SessionPort>(uint16_t port)>;

}  // namespace nxrt::vnet
