// SPDX-License-Identifier: GPL-3.0-or-later
// [net] protocol = native: a host-side port on the co-op module's session network (vnet::SessionPort; ldn_backend.h open_port). It is
// one of the module's virtual sockets, the same kind the game's datagrams use, so it reaches every member over whatever carries the
// session (TCP control links, UDP, the relay). The socket takes one of the module's virtual descriptors (0x100..) while it is open.
#include <atomic>
#include <memory>

#include "coop.h"
#include "ldn_native.h"
#include "nxrt/log.h"
#include "../net/vnet.h"

namespace nxrt::services::ldn_impl {
namespace {

constexpr int kSockNonblock = 0x20000000;   // nn::socket's (FreeBSD) values, as coop.h takes them
constexpr int kSolSocket = 0xFFFF, kSoBroadcast = 0x20;
constexpr int kMsgDontwait = 0x80;

// BSD sockaddr_in: len, family, port and address big-endian.
void put_sa(uint8_t (&sa)[16], uint32_t ip, uint16_t port) {
    sa[0] = 16;
    sa[1] = 2;
    sa[2] = static_cast<uint8_t>(port >> 8);
    sa[3] = static_cast<uint8_t>(port);
    sa[4] = static_cast<uint8_t>(ip >> 24);
    sa[5] = static_cast<uint8_t>(ip >> 16);
    sa[6] = static_cast<uint8_t>(ip >> 8);
    sa[7] = static_cast<uint8_t>(ip);
    for (int i = 8; i < 16; ++i) sa[i] = 0;
}

uint32_t own_ip(coop_node* n) {
    uint32_t ip = 0;
    return coop_ldn_ipv4(n, &ip, nullptr) == 0 ? ip : 0;   // no side effects (unlike the NetworkInfo the game reads)
}

class NativePort final : public vnet::SessionPort {
public:
    NativePort(coop_node* n, int fd, uint32_t ip, uint16_t port) : n_(n), fd_(fd), ip_(ip), port_(port) {}
    ~NativePort() override { coop_vnet_close(n_, fd_); }

    uint32_t local_ip() const override {
        const uint32_t ip = own_ip(n_);
        return ip == ip_ ? ip : 0;
    }
    // The module numbers nodes from the host: node 0 is subnet .1 (coop_int.h vip_of), and the host is node 0.
    uint32_t host_ip() const override { return (ip_ & 0xFFFFFF00u) | 1u; }

    bool send(uint32_t dst_ip, const void* p, size_t n) override {
        uint8_t sa[16];
        put_sa(sa, dst_ip, port_);
        return coop_vnet_sendto(n_, fd_, p, n, 0, sa, sizeof sa) >= 0;
    }

    int recv(void* p, size_t cap, uint32_t* src_ip, int timeout_ms) override {
        if (timeout_ms > 0) {
            coop_pollfd pf{fd_, COOP_POLLIN, 0};
            const int r = coop_vnet_poll(n_, &pf, 1, timeout_ms);
            if (r < 0) return -1;
            if (r == 0) return 0;
        }
        uint8_t sa[16] = {};
        uint32_t salen = sizeof sa;
        const int64_t r = coop_vnet_recvfrom(n_, fd_, p, cap, kMsgDontwait, sa, &salen);
        if (r == -COOP_EAGAIN) return 0;
        if (r < 0) return -1;
        *src_ip = (uint32_t{sa[4]} << 24) | (uint32_t{sa[5]} << 16) | (uint32_t{sa[6]} << 8) | sa[7];
        return static_cast<int>(r);
    }

private:
    coop_node* n_;
    int fd_;
    uint32_t ip_;
    uint16_t port_;
};

}  // namespace

std::unique_ptr<vnet::SessionPort> open_native_port(coop_node* n, uint16_t port) {
    if (!n) return nullptr;
    const uint32_t ip = own_ip(n);
    if (!ip) return nullptr;
    static std::atomic<bool> warned{false};   // one line per run: the opener is retried while in the session
    const int fd = coop_vnet_socket(n, 2, 2 | kSockNonblock, 17);
    if (fd < 0) {
        if (!warned.exchange(true)) NXLOG_W("net", "coop channel: no virtual socket for port %u (error %d)", port, -fd);
        return nullptr;
    }
    const int one = 1;
    coop_vnet_setsockopt(n, fd, kSolSocket, kSoBroadcast, &one, sizeof one);
    uint8_t sa[16];
    put_sa(sa, 0, port);
    if (const int r = coop_vnet_bind(n, fd, sa, sizeof sa); r < 0) {
        coop_vnet_close(n, fd);
        if (!warned.exchange(true)) NXLOG_W("net", "coop channel: virtual port %u not bound (error %d)", port, -r);
        return nullptr;
    }
    return std::make_unique<NativePort>(n, fd, ip, port);
}

}  // namespace nxrt::services::ldn_impl
