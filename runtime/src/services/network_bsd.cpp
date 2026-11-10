// SPDX-License-Identifier: GPL-3.0-or-later
// bsd (bsd:u, bsd:s, bsd:a): the socket layer, a thin adapter over the vsock table and the co-op module's virtual sockets. See network.cpp.
#include "network_internal.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"
#include "coop.h"
#include "ldn.h"
#include "services_internal.h"

namespace nxrt::services::netsvc {

namespace {

// =============================================================================================================================================
// bsd: bsd:u, bsd:s, bsd:a (IClient). Switchbrew "Sockets services". The UDP commands are a thin adapter over the vsock socket table
// (net/vsock.h: UDP datagrams on the vnet link ldn brings up); SOCK_STREAM and the other domains still answer ENETDOWN, and every other
// socket-level command returns -1 with errno ENETDOWN, or the success pair where the command is a control call. bsdcfg has an empty
// command table (ScarletNX bsd.swipc).
// Layouts from libnx nx/source/services/bsd.c (ISC) and Ryujinx Sockets/Bsd/IClient.cs (MIT): input (i32 fd, i32 flags/level/...) raw;
// data and addresses in AutoSelect buffers (type 0x21 in = A or X, 0x22 out = B or C: in_buf()/out_buf() take either); output (i32 ret,
// u32 errno), then u32 addrlen / optlen for RecvFrom, GetSockName, GetPeerName and GetSockOpt. The socket table is process-wide (every
// bsd session sees the same descriptors).
// sockaddr_in on the Switch is FreeBSD's: u8 sin_len (16), u8 sin_family (AF_INET 2), be16 port, be32 address, 8 zero bytes.
// =============================================================================================================================================

constexpr size_t kSockaddrSize = 16;

// (i32 ret, u32 errno) and the optional trailing u32.
uint32_t bsd_ret(ipc::Response& rsp, const vsock::Ret& r) {
    rsp.push(r.ret);
    rsp.push(r.err);
    return 0;
}

// Reads a sockaddr_in from an input buffer. 0 or an errno (EINVAL: too short, EAFNOSUPPORT: not AF_INET).
uint32_t read_sockaddr(const ipc::Buffer& b, vsock::Addr& a) {
    uint8_t sa[kSockaddrSize] = {};
    if (buf_read(b, sa, sizeof sa) < 8) return vsock::kEinval;
    if (sa[1] != vsock::kAfInet) return vsock::kEafnosupport;
    a.port = static_cast<uint16_t>((sa[2] << 8) | sa[3]);
    a.ip = (uint32_t{sa[4]} << 24) | (uint32_t{sa[5]} << 16) | (uint32_t{sa[6]} << 8) | sa[7];
    return 0;
}

// Writes a sockaddr_in to an output buffer; returns the bytes written (the addrlen answer).
uint32_t write_sockaddr(const ipc::Buffer& b, const vsock::Addr& a) {
    const uint8_t sa[kSockaddrSize] = {static_cast<uint8_t>(kSockaddrSize), static_cast<uint8_t>(vsock::kAfInet),
                                       static_cast<uint8_t>(a.port >> 8), static_cast<uint8_t>(a.port),
                                       static_cast<uint8_t>(a.ip >> 24), static_cast<uint8_t>(a.ip >> 16),
                                       static_cast<uint8_t>(a.ip >> 8), static_cast<uint8_t>(a.ip)};
    return static_cast<uint32_t>(buf_write(b, sa, sizeof sa));
}

// A command whose sockaddr input failed to parse answers -1 with that errno, unless the descriptor is bad (EBADF comes first).
vsock::Ret addr_fail(int fd, uint32_t e) { return {-1, vsock::stack().is_socket(fd) ? e : vsock::kEbadf}; }

uint32_t bsd_send(ipc::Response& rsp, int fd, int flags, const ipc::Buffer& data, const vsock::Addr* to) {
    return bsd_ret(rsp, vsock::stack().sendto(fd, buf_ptr(data), static_cast<size_t>(data.size), flags, to));
}

uint32_t bsd_recv(ipc::Response& rsp, int fd, int flags, const ipc::Buffer& data, vsock::Addr* from) {
    return bsd_ret(rsp, vsock::stack().recvfrom(fd, buf_mut(data), static_cast<size_t>(data.size), flags, from));
}

// ---- [net] protocol = native: the co-op module's virtual sockets (third_party/pokopia-coop coop.h, after pokopia-native's
// windows/src/coop/nxnative/sock_glue.c). AF_INET datagram sockets made while its ldn is initialized are its descriptors (0x100..0x11F);
// every command on one of them goes to coop_vnet_*, everything else stays on the vsock table. The module answers >= 0 or -errno in
// FreeBSD numbers; bsd answers Linux numbers (as vsock does), so they are translated. It has no connected sockets: Send/Write on one
// answers EDESTADDRREQ, Connect, GetPeerName and Shutdown ENETDOWN (pokopia-native's answer). Poll and Select wait on the module when
// every descriptor in the set is one of its; any other set keeps the offline answer (-1, ENETDOWN).
static_assert(vsock::kMaxSockets <= COOP_FD_BASE, "vsock descriptors (0..kMaxSockets-1) must stay below the co-op module's");

constexpr int kCoopFamily = 2, kCoopDgram = 2;
constexpr int kBsdONonblock = 4;   // FreeBSD O_NONBLOCK (the module's); vsock::kONonblock is Linux's 0x800: Fcntl accepts either

uint32_t coop_errno(int64_t e) {
    switch (e) {
        case COOP_EAGAIN: return vsock::kEagain;
        case 39: return vsock::kEdestaddrreq;   // EDESTADDRREQ
        case COOP_EMSGSIZE: return vsock::kEmsgsize;
        case COOP_ENOPROTOOPT: return 92;       // Linux ENOPROTOOPT
        case COOP_EADDRINUSE: return vsock::kEaddrinuse;
        case COOP_EADDRNOTAVAIL: return vsock::kEaddrnotavail;
        case COOP_ENETDOWN: return vsock::kEnetdown;
        case COOP_ENETUNREACH: return vsock::kEnetunreach;
        default: return static_cast<uint32_t>(e);   // EBADF 9, EACCES 13, EINVAL 22, EMFILE 24: the same in both
    }
}
vsock::Ret coop_r(int64_t r) { return r >= 0 ? vsock::Ret{static_cast<int32_t>(r), 0} : vsock::Ret{-1, coop_errno(-r)}; }

// The co-op node when fd is one of its descriptors, else null.
coop_node* coop_fd(int fd) {
    coop_node* n = ldn_native_node();
    return n && fd >= COOP_FD_BASE && coop_vnet_is(n, fd) ? n : nullptr;
}

uint32_t coop_send(ipc::Response& rsp, coop_node* n, int fd, int flags, const ipc::Buffer& data, const ipc::Buffer* to) {
    uint8_t sa[kSockaddrSize] = {};
    uint32_t salen = 0;
    if (to) salen = static_cast<uint32_t>(buf_read(*to, sa, sizeof sa));
    return bsd_ret(rsp, coop_r(coop_vnet_sendto(n, fd, buf_ptr(data), data.addr ? data.size : 0, flags, salen ? sa : nullptr, salen)));
}

uint32_t coop_recv(ipc::Response& rsp, coop_node* n, int fd, int flags, const ipc::Buffer& data, const ipc::Buffer* from) {
    uint8_t sa[kSockaddrSize] = {};
    uint32_t salen = sizeof sa;
    const vsock::Ret r = coop_r(coop_vnet_recvfrom(n, fd, buf_mut(data), data.addr ? data.size : 0, flags, sa, &salen));
    bsd_ret(rsp, r);
    if (from) rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(buf_write(*from, sa, salen < sizeof sa ? salen : sizeof sa)));
    return 0;
}

// Poll (libnx bsdPoll: u32 nfds, s32 timeout; the pollfd array in buffer 0 in and out, 8 bytes each: i32 fd, i16 events, i16 revents).
uint32_t bsd_poll(ipc::Request& req, ipc::Response& rsp) {
    const uint32_t nfds = req.arg<uint32_t>(0);
    const int32_t timeout = req.arg<int32_t>(4);
    coop_node* n = ldn_native_node();
    std::vector<coop_pollfd> fds(nfds <= 4096 ? nfds : 0);
    const uint64_t want = fds.size() * sizeof(coop_pollfd);
    bool virt = n && !fds.empty() && buf_read(req.in_buf(0), fds.data(), want) == want;
    bool any = false;
    for (const coop_pollfd& p : fds) {
        if (p.fd < 0) continue;
        any = true;
        if (!virt || !coop_fd(p.fd)) virt = false;
    }
    if (!virt || !any) return bsd_ret(rsp, {-1, kBsdNetDown});
    const vsock::Ret r = coop_r(coop_vnet_poll(n, fds.data(), fds.size(), timeout));
    if (!r.err) buf_write(req.out_buf(0), fds.data(), want);
    return bsd_ret(rsp, r);
}

// Select (libnx bsdSelect: u32 nfds, timeval {i64 sec, i64 usec} at 8, bool null-timeout at 0x18; the read, write and except fd_sets
// (1024 bits) in buffers 0..2 in and 0..2 out, an absent set as an empty buffer).
uint32_t bsd_select(ipc::Request& req, ipc::Response& rsp) {
    const int32_t nfds = req.arg<int32_t>(0);
    const int64_t sec = req.arg<int64_t>(8), usec = req.arg<int64_t>(0x10);
    const bool no_timeout = req.arg<uint8_t>(0x18) != 0;
    coop_node* n = ldn_native_node();
    uint64_t sets[3][16] = {};
    bool have[3] = {};
    bool virt = n != nullptr, any = false;
    for (int k = 0; k < 3; ++k) {
        const ipc::Buffer b = req.in_buf(static_cast<size_t>(k));
        if (!b.addr || !b.size) continue;
        have[k] = true;
        buf_read(b, sets[k], sizeof sets[k]);
        for (int fd = 0; fd < nfds && fd < 1024; ++fd) {
            if (!(sets[k][fd >> 6] >> (fd & 63) & 1)) continue;
            any = true;
            if (!virt || !coop_fd(fd)) virt = false;
        }
    }
    if (!virt || !any) return bsd_ret(rsp, {-1, kBsdNetDown});
    int64_t ms = no_timeout ? -1 : sec * 1000 + usec / 1000;
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    const vsock::Ret r = coop_r(coop_vnet_select(n, nfds, have[0] ? sets[0] : nullptr, have[1] ? sets[1] : nullptr,
                                                 have[2] ? sets[2] : nullptr, static_cast<int>(ms)));
    if (!r.err)
        for (int k = 0; k < 3; ++k)
            if (have[k]) buf_write(req.out_buf(static_cast<size_t>(k)), sets[k], sizeof sets[k]);
    return bsd_ret(rsp, r);
}

void add_bsd_udp(ipc::Service& s) {
    auto socket = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int domain = req.arg<int32_t>(0), type = req.arg<int32_t>(4), proto = req.arg<int32_t>(8);
        coop_node* n = ldn_native_node();
        if (n && domain == kCoopFamily && (type & 0xFFFF) == kCoopDgram && coop_ldn_state(n) != COOP_LS_NONE)
            return bsd_ret(rsp, coop_r(coop_vnet_socket(n, domain, type, proto)));
        return bsd_ret(rsp, vsock::stack().socket(domain, type, proto));
    };
    cmd(s, "bsd", 2, "Socket", socket);
    cmd(s, "bsd", 3, "SocketExempt", socket);
    cmd(s, "bsd", 8, "Recv", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_recv(rsp, n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.out_buf(0), nullptr);
        return bsd_recv(rsp, req.arg<int32_t>(0), req.arg<int32_t>(4), req.out_buf(0), nullptr);
    });
    cmd(s, "bsd", 9, "RecvFrom", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) {
            const ipc::Buffer from = req.out_buf(1);
            return coop_recv(rsp, n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.out_buf(0), &from);
        }
        vsock::Addr from;
        const vsock::Ret r = vsock::stack().recvfrom(req.arg<int32_t>(0), buf_mut(req.out_buf(0)), static_cast<size_t>(req.out_buf(0).size),
                                                     req.arg<int32_t>(4), &from);
        bsd_ret(rsp, r);
        rsp.push(r.err ? uint32_t{0} : write_sockaddr(req.out_buf(1), from));
        return 0;
    });
    cmd(s, "bsd", 10, "Send", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_send(rsp, n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.in_buf(0), nullptr);
        return bsd_send(rsp, req.arg<int32_t>(0), req.arg<int32_t>(4), req.in_buf(0), nullptr);
    });
    cmd(s, "bsd", 11, "SendTo", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int fd = req.arg<int32_t>(0);
        if (coop_node* n = coop_fd(fd)) {
            const ipc::Buffer to = req.in_buf(1);
            return coop_send(rsp, n, fd, req.arg<int32_t>(4), req.in_buf(0), &to);
        }
        vsock::Addr to;
        if (const uint32_t e = read_sockaddr(req.in_buf(1), to)) return bsd_ret(rsp, addr_fail(fd, e));
        ask_connect(to);
        return bsd_send(rsp, fd, req.arg<int32_t>(4), req.in_buf(0), &to);
    });
    cmd(s, "bsd", 13, "Bind", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int fd = req.arg<int32_t>(0);
        if (coop_node* n = coop_fd(fd)) {
            uint8_t sa[kSockaddrSize] = {};
            const uint32_t len = static_cast<uint32_t>(buf_read(req.in_buf(0), sa, sizeof sa));
            return bsd_ret(rsp, coop_r(coop_vnet_bind(n, fd, sa, len)));
        }
        vsock::Addr a;
        if (const uint32_t e = read_sockaddr(req.in_buf(0), a)) return bsd_ret(rsp, addr_fail(fd, e));
        return bsd_ret(rsp, vsock::stack().bind(fd, a));
    });
    cmd(s, "bsd", 14, "Connect", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int fd = req.arg<int32_t>(0);
        vsock::Addr a;
        if (read_sockaddr(req.in_buf(0), a) == 0) ask_connect(a);
        if (coop_fd(fd)) return bsd_ret(rsp, {-1, kBsdNetDown});
        if (const uint32_t e = read_sockaddr(req.in_buf(0), a)) return bsd_ret(rsp, addr_fail(fd, e));
        return bsd_ret(rsp, vsock::stack().connect(fd, a));
    });
    auto name = [](bool peer) {
        return [peer](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            vsock::Addr a;
            const int fd = req.arg<int32_t>(0);
            if (coop_node* n = coop_fd(fd)) {
                if (peer) {
                    bsd_ret(rsp, {-1, kBsdNetDown});
                    rsp.push(uint32_t{0});
                    return 0;
                }
                uint8_t sa[kSockaddrSize] = {};
                uint32_t len = sizeof sa;
                const vsock::Ret r = coop_r(coop_vnet_getsockname(n, fd, sa, &len));
                bsd_ret(rsp, r);
                rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(buf_write(req.out_buf(0), sa, len < sizeof sa ? len : sizeof sa)));
                return 0;
            }
            const vsock::Ret r = peer ? vsock::stack().getpeername(fd, &a) : vsock::stack().getsockname(fd, &a);
            bsd_ret(rsp, r);
            rsp.push(r.err ? uint32_t{0} : write_sockaddr(req.out_buf(0), a));
            return 0;
        };
    };
    cmd(s, "bsd", 15, "GetPeerName", name(true));
    cmd(s, "bsd", 16, "GetSockName", name(false));
    cmd(s, "bsd", 17, "GetSockOpt", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const ipc::Buffer out = req.out_buf(0);
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) {
            std::vector<uint8_t> v(static_cast<size_t>(out.size < 256 ? out.size : 256));
            uint32_t len = static_cast<uint32_t>(v.size());
            const vsock::Ret r = coop_r(coop_vnet_getsockopt(n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8),
                                                             v.empty() ? nullptr : v.data(), &len));
            bsd_ret(rsp, r);
            rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(buf_write(out, v.data(), len)));
            return 0;
        }
        size_t n = static_cast<size_t>(out.size);
        const vsock::Ret r = vsock::stack().getsockopt(req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8), buf_mut(out), &n);
        bsd_ret(rsp, r);
        rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(n));
        return 0;
    });
    cmd(s, "bsd", 20, "Fcntl", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) {
            const int c = req.arg<int32_t>(4);
            int v = req.arg<int32_t>(8);
            if (c == vsock::kFSetFl && (v & vsock::kONonblock)) v |= kBsdONonblock;
            vsock::Ret r = coop_r(coop_vnet_fcntl(n, req.arg<int32_t>(0), c, v));
            if (c == vsock::kFGetFl && !r.err && (r.ret & kBsdONonblock)) r.ret |= vsock::kONonblock;
            return bsd_ret(rsp, r);
        }
        return bsd_ret(rsp, vsock::stack().fcntl(req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8)));
    });
    cmd(s, "bsd", 21, "SetSockOpt", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const ipc::Buffer in = req.in_buf(0);
        if (coop_node* n = coop_fd(req.arg<int32_t>(0)))
            return bsd_ret(rsp, coop_r(coop_vnet_setsockopt(n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8), buf_ptr(in),
                                                            static_cast<uint32_t>(in.addr ? in.size : 0))));
        return bsd_ret(rsp, vsock::stack().setsockopt(req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8), buf_ptr(in),
                                                      static_cast<size_t>(in.size)));
    });
    cmd(s, "bsd", 22, "Shutdown", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, {-1, kBsdNetDown});
        return bsd_ret(rsp, vsock::stack().shutdown(req.arg<int32_t>(0), req.arg<int32_t>(4)));
    });
    cmd(s, "bsd", 24, "Write", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_send(rsp, n, req.arg<int32_t>(0), 0, req.in_buf(0), nullptr);
        return bsd_send(rsp, req.arg<int32_t>(0), 0, req.in_buf(0), nullptr);
    });
    cmd(s, "bsd", 25, "Read", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_recv(rsp, n, req.arg<int32_t>(0), 0, req.out_buf(0), nullptr);
        return bsd_recv(rsp, req.arg<int32_t>(0), 0, req.out_buf(0), nullptr);
    });
    cmd(s, "bsd", 26, "Close", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, coop_r(coop_vnet_close(n, req.arg<int32_t>(0))));
        return bsd_ret(rsp, vsock::stack().close(req.arg<int32_t>(0)));
    });
    cmd(s, "bsd", 5, "Select", bsd_select);
    cmd(s, "bsd", 6, "Poll", bsd_poll);
}

}  // namespace

std::shared_ptr<ipc::Service> make_bsd() {
    return make_iface("nn::socket::sf::IClient", [](ipc::Service& s) {
        cmd(s, "bsd", 0, "RegisterClient", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);                  // the transfer memory is not used
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        add_bsd_udp(s);
        rows(s, "bsd", {
            {1, "StartMonitoring", K::Ok},
            {4, "Open", K::Errno},
            {7, "Sysctl", K::Errno, 0, 1},
            {12, "Accept", K::Errno, 0, 1},
            {18, "Listen", K::Errno},
            {19, "Ioctl", K::Errno},
            {23, "ShutdownAllSockets", K::Errno},
            {27, "DuplicateSocket", K::Errno},
            {28, "GetResourceStatistics", K::Zeros, 0, 8},
            {29, "RecvMMsg", K::Errno},
            {30, "SendMMsg", K::Errno},
            {31, "EventFd", K::Errno},
            {32, "RegisterResourceStatisticsName", K::U32, 0},
            {33, "RegisterClientShared", K::U32, 0},
            {34, "GetSocketStatistics", K::Errno},
            {35, "NifIoctl", K::Errno},
            {36, "Cmd36", K::U32, 0},            // Switchbrew: 4 bytes out, stubbed to 0 on NX
            {37, "SetSocketBufferAttribute", K::Errno},
            {38, "Cmd38", K::Errno},            // no name in Switchbrew's table
            {39, "NoSfEventFd", K::Errno},
            {40, "ShutdownAllSockets", K::Errno},
            {41, "Cmd41", K::Errno},
            {42, "SendMMsgNoMem", K::Errno},
            {43, "RecvMMsgNoMem", K::Errno},
            {44, "Cmd44", K::Errno},
            {200, "SetThreadCoreMask", K::Ok},
            {201, "GetThreadCoreMask", K::U32, 0},
        });
    });
}

}  // namespace nxrt::services::netsvc
