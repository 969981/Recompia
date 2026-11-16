// SPDX-License-Identifier: GPL-3.0-or-later
// [net] protocol = linkplay: a host-side port on the relay room this process is in (vnet::SessionPort; ldn_backend.h open_port). It is a
// socket of the process socket table (vsock::stack()), the one the game's datagrams use, so it follows the current link and reaches every
// member through the relay. The socket takes one descriptor of that table while it is open.
#include <atomic>
#include <memory>

#include "ldn_linkplay.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"

namespace nxrt::services::ldn_impl {
namespace {

uint32_t link_ip() {
    const auto l = vnet::current_link();
    return l ? l->local_ip() : 0;
}

class LinkplayPort final : public vnet::SessionPort {
public:
    LinkplayPort(int fd, uint32_t ip, uint16_t port) : fd_(fd), ip_(ip), port_(port) {}
    ~LinkplayPort() override { vsock::stack().close(fd_); }

    uint32_t local_ip() const override {
        const uint32_t ip = link_ip();
        return ip == ip_ ? ip : 0;
    }
    // Room semantics (net/lobby.h): the host is node 0, .1 of the room's /24.
    uint32_t host_ip() const override { return (ip_ & 0xFFFFFF00u) | 1u; }

    bool send(uint32_t dst_ip, const void* p, size_t n) override {
        const vsock::Addr to{dst_ip, port_};
        return vsock::stack().sendto(fd_, p, n, 0, &to).ret >= 0;
    }

    int recv(void* p, size_t cap, uint32_t* src_ip, int timeout_ms) override {
        vsock::Stack& st = vsock::stack();
        if (timeout_ms > 0 && timeout_ms != timeout_ms_) {
            const int64_t tv[2] = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            st.setsockopt(fd_, vsock::kSolSocket, vsock::kSoRcvTimeo, tv, sizeof tv);
            timeout_ms_ = timeout_ms;
        }
        vsock::Addr from;
        const vsock::Ret r = st.recvfrom(fd_, p, cap, timeout_ms > 0 ? 0 : vsock::kMsgDontwait, &from);
        if (r.ret < 0) return r.err == vsock::kEagain ? 0 : -1;
        *src_ip = from.ip;
        return r.ret;
    }

private:
    int fd_;
    uint32_t ip_;
    uint16_t port_;
    int timeout_ms_ = 0;   // the socket's receive timeout now (0: none set yet)
};

}  // namespace

std::unique_ptr<vnet::SessionPort> open_linkplay_port(uint16_t port) {
    const uint32_t ip = link_ip();
    if (!ip) return nullptr;
    static std::atomic<bool> warned{false};   // one line per run: the opener is retried while in the room
    vsock::Stack& st = vsock::stack();
    const vsock::Ret s = st.socket(vsock::kAfInet, vsock::kSockDgram, vsock::kIpprotoUdp);
    if (s.ret < 0) {
        if (!warned.exchange(true)) NXLOG_W("net", "coop channel: no socket for port %u (errno %u)", port, s.err);
        return nullptr;
    }
    const int one = 1;
    st.setsockopt(s.ret, vsock::kSolSocket, vsock::kSoBroadcast, &one, sizeof one);
    if (const vsock::Ret b = st.bind(s.ret, {0, port}); b.ret < 0) {
        st.close(s.ret);
        if (!warned.exchange(true)) NXLOG_W("net", "coop channel: port %u not bound (errno %u)", port, b.err);
        return nullptr;
    }
    return std::make_unique<LinkplayPort>(s.ret, ip, port);
}

}  // namespace nxrt::services::ldn_impl
