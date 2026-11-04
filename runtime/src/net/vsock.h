// SPDX-License-Identifier: GPL-3.0-or-later
// The socket table behind the bsd HLE (multiplayer phase 1): UDP sockets over a vnet::Link. Game-independent; the IPC side is
// make_bsd() in services/network.cpp. Every call answers (ret, errno) the way bsd:u does: ret -1 with a Linux errno number on failure
// (Switchbrew "Sockets services"; Ryujinx Sockets/Bsd uses LinuxError the same way).
//
// What is modelled (Linux/FreeBSD UDP behaviour):
//   - AF_INET + SOCK_DGRAM only (SOCK_NONBLOCK / SOCK_CLOEXEC flags in the type). SOCK_STREAM and other domains: ENETDOWN, as the offline
//     bsd answered before. Descriptors are the lowest free from 0 (as Ryujinx hands them out), at most 128.
//   - bind (port 0 = ephemeral from 49152, address 0.0.0.0 or the link's IP), connect (sets the default peer), sendto/send, recvfrom/recv
//     (MSG_DONTWAIT, MSG_PEEK, O_NONBLOCK, SO_RCVTIMEO; a blocking call waits until a datagram arrives), getsockname/getpeername, shutdown,
//     close, fcntl F_GETFL/F_SETFL O_NONBLOCK, set/getsockopt at SOL_SOCKET (SO_BROADCAST, SO_REUSEADDR, SO_REUSEPORT, SO_RCVBUF, SO_SNDBUF,
//     SO_RCVTIMEO, SO_SNDTIMEO, SO_TYPE, SO_ERROR); any other option is accepted, kept for getsockopt and logged once.
//   - Delivery: a datagram goes to sockets bound to its port with address 0.0.0.0 or the destination IP. Unicast reaches one socket (the
//     exact-address one first), a broadcast (x.y.z.255, or 255.255.255.255 sent as the link's /24 broadcast) every 0.0.0.0 socket on the
//     port (Linux: receiving broadcast needs no SO_BROADCAST; sending does, else EACCES). The sender's own broadcast is not looped back
//     (a real NIC does not); a unicast to the node's own IP is. Receive queue: 64 datagrams per socket, the oldest dropped.
//   - Errors: bad fd EBADF; no link (or the link is down) ENETUNREACH; destination outside 169.254.0.0/16 ENETDOWN; > 4096 bytes EMSGSIZE;
//     port in use without SO_REUSEADDR/SO_REUSEPORT on both EADDRINUSE; nothing queued and non-blocking EAGAIN.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "vnet.h"

namespace nxrt::vsock {

// ---- Switch bsd constants (FreeBSD numbering for flags and options, Linux numbers for errno) ---------------------------------------------
constexpr int kAfInet = 2;
constexpr int kSockStream = 1, kSockDgram = 2;
constexpr int kSockNonblock = 0x20000000, kSockCloexec = 0x40000000;   // flags in Socket's type
constexpr int kIpprotoUdp = 17;
constexpr int kMsgPeek = 0x2, kMsgDontwait = 0x80;
constexpr int kSolSocket = 0xFFFF;
constexpr int kSoReuseAddr = 0x4, kSoBroadcast = 0x20, kSoReusePort = 0x200;
constexpr int kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoSndTimeo = 0x1005, kSoRcvTimeo = 0x1006, kSoError = 0x1007, kSoType = 0x1008;
constexpr int kFGetFl = 3, kFSetFl = 4, kONonblock = 0x800;
constexpr int kShutRd = 0, kShutWr = 1, kShutRdWr = 2;

constexpr uint32_t kEbadf = 9, kEagain = 11, kEacces = 13, kEinval = 22, kEmfile = 24, kEpipe = 32, kEdestaddrreq = 89, kEmsgsize = 90,
                   kEprotonosupport = 93, kEopnotsupp = 95, kEafnosupport = 97, kEaddrinuse = 98, kEaddrnotavail = 99, kEnetdown = 100,
                   kEnetunreach = 101, kEnotconn = 107;

constexpr int kMaxSockets = 128;
constexpr size_t kQueueCap = 64;
constexpr uint16_t kEphemeralFirst = 49152;

struct Addr {
    uint32_t ip = 0;     // host order
    uint16_t port = 0;   // host order
};

// (ret, errno): ret -1 and err != 0 on failure.
struct Ret {
    int32_t ret = 0;
    uint32_t err = 0;
};

class Stack {
public:
    // fixed: the link this table always uses. Null: it follows vnet::current_link() (only the process table, stack(), does that).
    explicit Stack(std::shared_ptr<vnet::Link> fixed = nullptr);
    ~Stack();
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;

    Ret socket(int domain, int type, int protocol);
    Ret bind(int fd, Addr a);
    Ret connect(int fd, Addr a);
    Ret sendto(int fd, const void* p, size_t n, int flags, const Addr* to);   // to null: the connected peer (send)
    Ret recvfrom(int fd, void* p, size_t n, int flags, Addr* from);           // from may be null (recv)
    Ret setsockopt(int fd, int level, int opt, const void* v, size_t n);
    Ret getsockopt(int fd, int level, int opt, void* v, size_t* n);
    Ret fcntl(int fd, int cmd, int arg);
    Ret getsockname(int fd, Addr* a);
    Ret getpeername(int fd, Addr* a);
    Ret shutdown(int fd, int how);
    Ret close(int fd);
    bool is_socket(int fd);
    void reset();   // closes every socket (waiting receivers return EBADF)

    void inbound(const vnet::Datagram& d);   // a datagram from the link

private:
    struct Sock;
    std::shared_ptr<vnet::Link> link() const;
    std::shared_ptr<Sock> get(int fd);
    bool port_free(uint16_t port, uint32_t ip, const Sock& s) const;   // caller holds mu_
    uint32_t autobind(Sock& s);                                        // caller holds mu_; 0 or errno
    Ret wait_recv(const std::shared_ptr<Sock>& s, void* p, size_t n, int flags, Addr* from);

    std::shared_ptr<vnet::Link> fixed_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<int, std::shared_ptr<Sock>> socks_;
    std::set<uint64_t> logged_opts_;   // (level << 32 | opt) of the unknown options already logged
    uint16_t next_port_ = kEphemeralFirst;
};

// The process-wide table the bsd services use. It follows vnet::current_link().
Stack& stack();

}  // namespace nxrt::vsock
