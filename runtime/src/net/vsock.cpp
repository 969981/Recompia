// SPDX-License-Identifier: GPL-3.0-or-later
// UDP socket table over a vnet::Link (see vsock.h). One mutex guards the table and every socket's state; one condition variable wakes
// blocking receivers. The link is never called with the mutex held (a hub delivers on the sending thread, back into inbound()).
#include "vsock.h"
#include "nxrt/text.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::vsock {

struct Stack::Sock {
    bool nonblock = false;
    bool bound = false;
    Addr local;
    bool connected = false;
    Addr peer;
    bool broadcast = false, reuse_addr = false, reuse_port = false;
    bool shut_rd = false, shut_wr = false;
    bool closed = false;
    int32_t rcvbuf = 42080, sndbuf = 9216;   // FreeBSD's UDP defaults (net.inet.udp.recvspace / maxdgram)
    int64_t rcvtimeo_us = 0;                 // 0 = wait forever
    uint8_t sndtimeo[16] = {};
    std::map<uint64_t, std::vector<uint8_t>> other_opts;   // (level << 32 | opt) -> value, for getsockopt
    std::deque<vnet::Datagram> q;
};

namespace {
Ret ok(int32_t v = 0) { return {v, 0}; }
Ret fail(uint32_t e) { return {-1, e}; }
constexpr uint32_t kLimitedBroadcast = 0xFFFFFFFFu;

// The payload's first kDumpBytes bytes as hex for the Debug udp lines ("|" every 16 bytes, "..." when cut). 192 covers Pokopia's KTGL
// join request (171 bytes with its 4-byte routing header, work/mp/join-reject-analysis.md). Only built when Debug is on (NXLOG_D).
constexpr size_t kDumpBytes = 192;
std::string hex_head(const std::vector<uint8_t>& b) { return text::hex_head(b, kDumpBytes); }
}  // namespace

Stack::Stack(std::shared_ptr<vnet::Link> fixed) : fixed_(std::move(fixed)) {
    if (fixed_) fixed_->set_receiver([this](const vnet::Datagram& d) { inbound(d); });
}

Stack::~Stack() {
    if (fixed_) fixed_->set_receiver(nullptr);
    reset();
}

std::shared_ptr<vnet::Link> Stack::link() const { return fixed_ ? fixed_ : vnet::current_link(); }

std::shared_ptr<Stack::Sock> Stack::get(int fd) {
    auto it = socks_.find(fd);
    return it == socks_.end() ? nullptr : it->second;
}

bool Stack::is_socket(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    return get(fd) != nullptr;
}

Ret Stack::socket(int domain, int type, int protocol) {
    const int base = type & 0x0FFFFFFF;
    if (domain != kAfInet || base != kSockDgram) return fail(kEnetdown);   // TCP and the rest: the network stays down for them
    if (protocol != 0 && protocol != kIpprotoUdp) return fail(kEprotonosupport);
    std::lock_guard<std::mutex> lk(mu_);
    int fd = 0;
    while (fd < kMaxSockets && socks_.count(fd)) ++fd;
    if (fd == kMaxSockets) return fail(kEmfile);
    auto s = std::make_shared<Sock>();
    s->nonblock = (type & kSockNonblock) != 0;
    socks_[fd] = s;
    return ok(fd);
}

bool Stack::port_free(uint16_t port, uint32_t ip, const Sock& s) const {
    for (const auto& [fd, o] : socks_) {
        if (o.get() == &s || !o->bound || o->local.port != port) continue;
        if (o->local.ip != 0 && ip != 0 && o->local.ip != ip) continue;     // different addresses do not overlap
        if ((s.reuse_addr && o->reuse_addr) || (s.reuse_port && o->reuse_port)) continue;
        return false;
    }
    return true;
}

uint32_t Stack::autobind(Sock& s) {
    if (s.bound) return 0;
    for (int i = 0; i < 65536 - kEphemeralFirst; ++i) {
        const uint16_t p = next_port_;
        next_port_ = next_port_ == 65535 ? kEphemeralFirst : static_cast<uint16_t>(next_port_ + 1);
        if (port_free(p, s.local.ip, s)) {
            s.local.port = p;
            s.bound = true;
            return 0;
        }
    }
    return kEaddrinuse;
}

Ret Stack::bind(int fd, Addr a) {
    const auto l = link();
    const uint32_t me = l ? l->local_ip() : 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (s->bound) return fail(kEinval);
    if (a.ip != 0 && (me == 0 || a.ip != me)) return fail(kEaddrnotavail);
    s->local.ip = a.ip;
    if (a.port == 0) return autobind(*s) ? fail(kEaddrinuse) : ok();
    if (!port_free(a.port, a.ip, *s)) return fail(kEaddrinuse);
    s->local.port = a.port;
    s->bound = true;
    return ok();
}

Ret Stack::connect(int fd, Addr a) {
    const auto l = link();
    const uint32_t me = l ? l->local_ip() : 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (!vnet::is_link_local(a.ip)) return fail(kEnetdown);
    if (me == 0) return fail(kEnetunreach);
    if (vnet::is_subnet_broadcast(a.ip) && !s->broadcast) return fail(kEacces);
    if (const uint32_t e = autobind(*s)) return fail(e);
    s->connected = true;
    s->peer = a;
    return ok();
}

Ret Stack::sendto(int fd, const void* p, size_t n, int flags, const Addr* to) {
    (void)flags;   // MSG_DONTWAIT: a send never blocks here
    const auto l = link();
    const uint32_t me = l ? l->local_ip() : 0;   // asked before taking mu_: a link may lock inside
    vnet::Datagram d;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto s = get(fd);
        if (!s) return fail(kEbadf);
        if (s->shut_wr) return fail(kEpipe);
        Addr dst;
        if (to) dst = *to;
        else if (s->connected) dst = s->peer;
        else return fail(kEdestaddrreq);
        if (n > vnet::kMaxDatagram) return fail(kEmsgsize);
        if (dst.ip == kLimitedBroadcast) {   // 255.255.255.255 leaves as the subnet broadcast
            if (me == 0) return fail(kEnetunreach);
            dst.ip = me | 0xFFu;
        }
        if (!vnet::is_link_local(dst.ip)) return fail(kEnetdown);
        if (me == 0) return fail(kEnetunreach);
        if (dst.port == 0) return fail(kEinval);
        if (vnet::is_subnet_broadcast(dst.ip) && !s->broadcast) return fail(kEacces);
        if (const uint32_t e = autobind(*s)) return fail(e);
        d.src_ip = me;
        d.src_port = s->local.port;
        d.dst_ip = dst.ip;
        d.dst_port = dst.port;
        d.data.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    }
    NXLOG_D("net", "udp fd %d send %s:%u -> %s:%u, %zu bytes: %s", fd, vnet::ip_str(d.src_ip).c_str(), d.src_port,
            vnet::ip_str(d.dst_ip).c_str(), d.dst_port, n, hex_head(d.data).c_str());
    if (d.dst_ip == d.src_ip) inbound(d);          // to our own address: the loopback path, not the wire
    else if (!l->send(d)) return fail(kEnetunreach);
    return ok(static_cast<int32_t>(n));
}

void Stack::inbound(const vnet::Datagram& d) {
    const bool bcast = vnet::is_subnet_broadcast(d.dst_ip);
    std::lock_guard<std::mutex> lk(mu_);
    std::shared_ptr<Sock> wild;   // unicast: an exact-address socket wins over a 0.0.0.0 one
    int wild_fd = -1;
    bool any = false;
    auto put = [&](int fd, Sock& s) {
        if (s.q.size() >= kQueueCap) s.q.pop_front();
        s.q.push_back(d);
        any = true;
        NXLOG_D("net", "udp fd %d recv %s:%u -> %s:%u, %zu bytes: %s", fd, vnet::ip_str(d.src_ip).c_str(), d.src_port,
                vnet::ip_str(d.dst_ip).c_str(), d.dst_port, d.data.size(), hex_head(d.data).c_str());
    };
    for (const auto& [fd, s] : socks_) {
        if (!s->bound || s->shut_rd || s->local.port != d.dst_port) continue;
        if (s->connected && (s->peer.ip != d.src_ip || s->peer.port != d.src_port)) continue;
        if (bcast) {
            if (s->local.ip == 0 || s->local.ip == d.dst_ip) put(fd, *s);
        } else if (s->local.ip == d.dst_ip) {
            put(fd, *s);
            wild = nullptr;
            break;
        } else if (s->local.ip == 0 && !wild) {
            wild = s;
            wild_fd = fd;
        }
    }
    if (wild) put(wild_fd, *wild);
    if (any) cv_.notify_all();
    else NXLOG_D("net", "udp drop %s:%u -> %s:%u: no socket on the port", vnet::ip_str(d.src_ip).c_str(), d.src_port,
                 vnet::ip_str(d.dst_ip).c_str(), d.dst_port);
}

Ret Stack::recvfrom(int fd, void* p, size_t n, int flags, Addr* from) {
    std::shared_ptr<Sock> s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s = get(fd);
        if (!s) return fail(kEbadf);
    }
    return wait_recv(s, p, n, flags, from);
}

Ret Stack::wait_recv(const std::shared_ptr<Sock>& s, void* p, size_t n, int flags, Addr* from) {
    std::unique_lock<std::mutex> lk(mu_);
    const bool nb = s->nonblock || (flags & kMsgDontwait);
    auto ready = [&] { return !s->q.empty() || s->closed || s->shut_rd; };
    if (!ready()) {
        if (nb) return fail(kEagain);
        if (s->rcvtimeo_us > 0) {
            if (!cv_.wait_for(lk, std::chrono::microseconds(s->rcvtimeo_us), ready)) return fail(kEagain);
        } else {
            cv_.wait(lk, ready);
        }
    }
    if (s->closed) return fail(kEbadf);
    if (s->q.empty()) return ok(0);   // shut down for reading: end of data
    const vnet::Datagram& d = s->q.front();
    const size_t m = d.data.size() < n ? d.data.size() : n;   // the rest of a longer datagram is lost, as on BSD
    if (m) std::memcpy(p, d.data.data(), m);
    if (from) *from = {d.src_ip, d.src_port};
    if (!(flags & kMsgPeek)) s->q.pop_front();
    return ok(static_cast<int32_t>(m));
}

Ret Stack::setsockopt(int fd, int level, int opt, const void* v, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    int32_t iv = 0;
    if (v && n >= 4) std::memcpy(&iv, v, 4);
    if (level == kSolSocket) {
        switch (opt) {
            case kSoBroadcast: s->broadcast = iv != 0; return ok();
            case kSoReuseAddr: s->reuse_addr = iv != 0; return ok();
            case kSoReusePort: s->reuse_port = iv != 0; return ok();
            case kSoRcvBuf: s->rcvbuf = iv; return ok();
            case kSoSndBuf: s->sndbuf = iv; return ok();
            case kSoRcvTimeo: {   // struct timeval {i64 tv_sec, i64 tv_usec}
                int64_t tv[2] = {0, 0};
                if (v && n >= sizeof tv) std::memcpy(tv, v, sizeof tv);
                s->rcvtimeo_us = tv[0] * 1000000 + tv[1];
                return ok();
            }
            case kSoSndTimeo:   // sends never block: kept for getsockopt only
                std::memset(s->sndtimeo, 0, sizeof s->sndtimeo);
                if (v) std::memcpy(s->sndtimeo, v, n < sizeof s->sndtimeo ? n : sizeof s->sndtimeo);
                return ok();
            default: break;
        }
    }
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | static_cast<uint32_t>(opt);
    const uint8_t* b = static_cast<const uint8_t*>(v);
    s->other_opts[key].assign(b, b + (v ? n : 0));
    if (logged_opts_.insert(key).second)
        NXLOG_I("net", "bsd setsockopt level 0x%x opt 0x%x (%zu bytes): accepted without effect", level, opt, n);
    return ok();
}

Ret Stack::getsockopt(int fd, int level, int opt, void* v, size_t* n) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    auto put_int = [&](int32_t x) {
        if (*n >= 4) {
            std::memcpy(v, &x, 4);
            *n = 4;
        }
        return *n >= 4 ? ok() : fail(kEinval);
    };
    if (level == kSolSocket) {
        switch (opt) {
            case kSoBroadcast: return put_int(s->broadcast ? opt : 0);   // FreeBSD answers the option bit, not 1
            case kSoReuseAddr: return put_int(s->reuse_addr ? opt : 0);
            case kSoReusePort: return put_int(s->reuse_port ? opt : 0);
            case kSoRcvBuf: return put_int(s->rcvbuf);
            case kSoSndBuf: return put_int(s->sndbuf);
            case kSoType: return put_int(kSockDgram);
            case kSoError: return put_int(0);
            case kSoRcvTimeo: {
                const int64_t tv[2] = {s->rcvtimeo_us / 1000000, s->rcvtimeo_us % 1000000};
                if (*n < sizeof tv) return fail(kEinval);
                std::memcpy(v, tv, sizeof tv);
                *n = sizeof tv;
                return ok();
            }
            case kSoSndTimeo:
                if (*n < sizeof s->sndtimeo) return fail(kEinval);
                std::memcpy(v, s->sndtimeo, sizeof s->sndtimeo);
                *n = sizeof s->sndtimeo;
                return ok();
            default: break;
        }
    }
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | static_cast<uint32_t>(opt);
    auto it = s->other_opts.find(key);
    if (it == s->other_opts.end()) return put_int(0);
    const size_t m = it->second.size() < *n ? it->second.size() : *n;
    if (m) std::memcpy(v, it->second.data(), m);
    *n = m;
    return ok();
}

Ret Stack::fcntl(int fd, int cmd, int arg) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (cmd == kFGetFl) return ok(s->nonblock ? kONonblock : 0);
    if (cmd == kFSetFl) {
        s->nonblock = (arg & kONonblock) != 0;
        return ok();
    }
    return fail(kEinval);
}

Ret Stack::getsockname(int fd, Addr* a) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    *a = s->local;   // 0.0.0.0:0 before bind, as on BSD
    return ok();
}

Ret Stack::getpeername(int fd, Addr* a) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (!s->connected) return fail(kEnotconn);
    *a = s->peer;
    return ok();
}

Ret Stack::shutdown(int fd, int how) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (how < kShutRd || how > kShutRdWr) return fail(kEinval);
    if (!s->connected) return fail(kEnotconn);   // BSD and Linux: an unconnected datagram socket has nothing to shut down
    if (how != kShutWr) s->shut_rd = true;
    if (how != kShutRd) s->shut_wr = true;
    cv_.notify_all();
    return ok();
}

Ret Stack::close(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = socks_.find(fd);
    if (it == socks_.end()) return fail(kEbadf);
    it->second->closed = true;
    socks_.erase(it);
    cv_.notify_all();
    return ok();
}

void Stack::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [fd, s] : socks_) s->closed = true;
    socks_.clear();
    logged_opts_.clear();
    next_port_ = kEphemeralFirst;
    cv_.notify_all();
}

Stack& stack() {
    static Stack* s = [] {
        auto* t = new Stack();   // never destroyed: guest threads may still be blocked in it at exit
        vnet::set_inbound([t](const vnet::Datagram& d) { t->inbound(d); });
        return t;
    }();
    return *s;
}

}  // namespace nxrt::vsock
