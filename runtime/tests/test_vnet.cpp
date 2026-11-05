// SPDX-License-Identifier: GPL-3.0-or-later
// The virtual LAN and its UDP socket table (multiplayer phase 1): two or three nodes on an in-process LoopbackHub, each with its own
// vsock::Stack, exchange datagrams the way two consoles on one ldn network do. Errno numbers are the Linux ones bsd:u answers.
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "../src/net/vnet.h"
#include "../src/net/vsock.h"
#include "test_framework.h"

NXTEST_GROUP(vnet)

using namespace nxrt;

namespace {
constexpr uint16_t kPort = 49530;
const uint32_t kIpA = vnet::ip(169, 254, 1, 1), kIpB = vnet::ip(169, 254, 1, 2), kIpC = vnet::ip(169, 254, 2, 1);

// A non-blocking UDP socket bound to 0.0.0.0:port, with SO_BROADCAST on (the game's socket).
int udp(vsock::Stack& s, uint16_t port = kPort) {
    const vsock::Ret r = s.socket(vsock::kAfInet, vsock::kSockDgram | vsock::kSockNonblock, 0);
    if (r.ret < 0) return -1;
    const int32_t on = 1;
    s.setsockopt(r.ret, vsock::kSolSocket, vsock::kSoBroadcast, &on, sizeof on);
    if (s.bind(r.ret, {0, port}).ret != 0) return -1;
    return r.ret;
}
vsock::Ret send_str(vsock::Stack& s, int fd, uint32_t ip, const char* text, uint16_t port = kPort) {
    const vsock::Addr to{ip, port};
    return s.sendto(fd, text, std::strlen(text), 0, &to);
}
}  // namespace

NXTEST(unicast_both_ways) {
    vnet::LoopbackHub hub;
    vsock::Stack a(hub.add(kIpA)), b(hub.add(kIpB));
    const int fa = udp(a), fb = udp(b);
    REQUIRE(fa >= 0 && fb >= 0);
    CHECK_EQ(send_str(a, fa, kIpB, "hello").ret, 5);
    char buf[64] = {};
    vsock::Addr from;
    vsock::Ret r = b.recvfrom(fb, buf, sizeof buf, 0, &from);
    CHECK_EQ(r.ret, 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);
    CHECK_EQ(from.ip, kIpA);
    CHECK_EQ(from.port, kPort);
    CHECK_EQ(send_str(b, fb, kIpA, "back").ret, 4);
    r = a.recvfrom(fa, buf, sizeof buf, 0, &from);
    CHECK_EQ(r.ret, 4);
    CHECK(std::memcmp(buf, "back", 4) == 0);
    CHECK_EQ(from.ip, kIpB);
    // A datagram to another port finds no socket and is dropped.
    CHECK_EQ(send_str(a, fa, kIpB, "lost", 1234).ret, 4);
    CHECK_EQ(b.recvfrom(fb, buf, sizeof buf, 0, &from).err, vsock::kEagain);
}

NXTEST(broadcast_reaches_others_not_self) {
    vnet::LoopbackHub hub;
    vsock::Stack a(hub.add(kIpA)), b(hub.add(kIpB)), c(hub.add(kIpC));   // c is on another /24
    const int fa = udp(a), fb = udp(b), fc = udp(c);
    REQUIRE(fa >= 0 && fb >= 0 && fc >= 0);
    CHECK_EQ(send_str(a, fa, vnet::ip(169, 254, 1, 255), "all").ret, 3);
    char buf[16] = {};
    vsock::Addr from;
    CHECK_EQ(b.recvfrom(fb, buf, sizeof buf, 0, &from).ret, 3);
    CHECK_EQ(from.ip, kIpA);
    CHECK_EQ(a.recvfrom(fa, buf, sizeof buf, 0, &from).err, vsock::kEagain);   // no loopback of our own broadcast
    CHECK_EQ(c.recvfrom(fc, buf, sizeof buf, 0, &from).err, vsock::kEagain);   // other subnet
    // 255.255.255.255 goes out as the subnet broadcast.
    CHECK_EQ(send_str(a, fa, 0xFFFFFFFFu, "lim").ret, 3);
    CHECK_EQ(b.recvfrom(fb, buf, sizeof buf, 0, &from).ret, 3);
    // Receiving needs no SO_BROADCAST, sending does (EACCES).
    const int plain = b.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;
    CHECK_EQ(send_str(b, plain, vnet::ip(169, 254, 1, 255), "no").err, vsock::kEacces);
}

NXTEST(eagain_and_nonblock_modes) {
    vnet::LoopbackHub hub;
    vsock::Stack a(hub.add(kIpA));
    const int fd = a.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;   // blocking
    REQUIRE(fd >= 0);
    CHECK_EQ(a.bind(fd, {0, kPort}).ret, 0);
    char buf[8];
    CHECK_EQ(a.recvfrom(fd, buf, sizeof buf, vsock::kMsgDontwait, nullptr).err, vsock::kEagain);
    CHECK_EQ(a.fcntl(fd, vsock::kFGetFl, 0).ret, 0);
    CHECK_EQ(a.fcntl(fd, vsock::kFSetFl, vsock::kONonblock).ret, 0);
    CHECK_EQ(a.fcntl(fd, vsock::kFGetFl, 0).ret, vsock::kONonblock);
    CHECK_EQ(a.recvfrom(fd, buf, sizeof buf, 0, nullptr).err, vsock::kEagain);
    // SO_RCVTIMEO on a blocking socket: EAGAIN after the timeout.
    CHECK_EQ(a.fcntl(fd, vsock::kFSetFl, 0).ret, 0);
    const int64_t tv[2] = {0, 20000};
    CHECK_EQ(a.setsockopt(fd, vsock::kSolSocket, vsock::kSoRcvTimeo, tv, sizeof tv).ret, 0);
    CHECK_EQ(a.recvfrom(fd, buf, sizeof buf, 0, nullptr).err, vsock::kEagain);
}

NXTEST(queue_keeps_newest_64) {
    vnet::LoopbackHub hub;
    vsock::Stack a(hub.add(kIpA)), b(hub.add(kIpB));
    const int fa = udp(a), fb = udp(b);
    REQUIRE(fa >= 0 && fb >= 0);
    for (int i = 0; i < 70; ++i) {
        const uint8_t v = static_cast<uint8_t>(i);
        const vsock::Addr to{kIpB, kPort};
        CHECK_EQ(a.sendto(fa, &v, 1, 0, &to).ret, 1);
    }
    uint8_t got = 0xFF;
    int n = 0;
    CHECK_EQ(b.recvfrom(fb, &got, 1, 0, nullptr).ret, 1);
    CHECK_EQ(got, uint8_t{6});   // 0..5 were dropped
    for (n = 1; b.recvfrom(fb, &got, 1, 0, nullptr).ret == 1; ++n) {}
    CHECK_EQ(n, 64);
    CHECK_EQ(got, uint8_t{69});
}

NXTEST(bind_rules) {
    vnet::LoopbackHub hub;
    vsock::Stack a(hub.add(kIpA));
    const int f1 = a.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;
    const int f2 = a.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;
    CHECK_EQ(a.bind(f1, {0, kPort}).ret, 0);
    CHECK_EQ(a.bind(f2, {0, kPort}).err, vsock::kEaddrinuse);
    CHECK_EQ(a.bind(f2, {kIpA, kPort}).err, vsock::kEaddrinuse);      // 0.0.0.0 overlaps every address
    CHECK_EQ(a.bind(f2, {kIpB, 1000}).err, vsock::kEaddrnotavail);   // not our address
    CHECK_EQ(a.bind(f1, {0, 1000}).err, vsock::kEinval);              // already bound
    // SO_REUSEADDR on both: allowed.
    const int32_t on = 1;
    const int f3 = a.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;
    const int f4 = a.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;
    a.setsockopt(f3, vsock::kSolSocket, vsock::kSoReuseAddr, &on, 4);
    a.setsockopt(f4, vsock::kSolSocket, vsock::kSoReuseAddr, &on, 4);
    CHECK_EQ(a.bind(f3, {0, 2000}).ret, 0);
    CHECK_EQ(a.bind(f4, {0, 2000}).ret, 0);
    // Port 0: an ephemeral port.
    CHECK_EQ(a.bind(f2, {0, 0}).ret, 0);
    vsock::Addr name;
    CHECK_EQ(a.getsockname(f2, &name).ret, 0);
    CHECK(name.port >= vsock::kEphemeralFirst);
    // Descriptors: the lowest free one comes back after close; a closed one is EBADF.
    CHECK_EQ(a.close(f1).ret, 0);
    CHECK_EQ(a.close(f1).err, vsock::kEbadf);
    CHECK_EQ(a.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret, f1);
}

NXTEST(errors_without_a_network) {
    vnet::set_current_link(nullptr);
    vsock::Stack s;   // follows the current link: none
    const int fd = s.socket(vsock::kAfInet, vsock::kSockDgram | vsock::kSockNonblock, 0).ret;
    REQUIRE(fd >= 0);
    CHECK_EQ(send_str(s, fd, kIpB, "x").err, vsock::kEnetunreach);
    CHECK_EQ(send_str(s, fd, vnet::ip(192, 168, 1, 2), "x").err, vsock::kEnetdown);   // off 169.254.0.0/16
    CHECK_EQ(s.socket(vsock::kAfInet, vsock::kSockStream, 0).err, vsock::kEnetdown);
    CHECK_EQ(s.socket(28, vsock::kSockDgram, 0).err, vsock::kEnetdown);                // AF_INET6
    CHECK_EQ(send_str(s, 77, kIpB, "x").err, vsock::kEbadf);
    std::vector<uint8_t> big(vnet::kMaxDatagram + 1);
    const vsock::Addr to{kIpB, kPort};
    CHECK_EQ(s.sendto(fd, big.data(), big.size(), 0, &to).err, vsock::kEmsgsize);
    // With a link up: ENETUNREACH turns into a send; off-subnet stays ENETDOWN.
    vnet::LoopbackHub hub;
    vnet::set_current_link(hub.add(kIpA));
    CHECK_EQ(send_str(s, fd, kIpB, "x").ret, 1);
    CHECK_EQ(send_str(s, fd, vnet::ip(10, 0, 0, 1), "x").err, vsock::kEnetdown);
    CHECK_EQ(s.sendto(fd, big.data(), vnet::kMaxDatagram, 0, &to).ret, static_cast<int32_t>(vnet::kMaxDatagram));
    vnet::set_current_link(nullptr);
}

NXTEST(blocking_recvfrom_wakes) {
    vnet::LoopbackHub hub;
    vsock::Stack a(hub.add(kIpA)), b(hub.add(kIpB));
    const int fa = udp(a);
    const int fb = b.socket(vsock::kAfInet, vsock::kSockDgram, 0).ret;   // blocking
    REQUIRE(fa >= 0 && fb >= 0);
    CHECK_EQ(b.bind(fb, {0, kPort}).ret, 0);
    vsock::Ret got;
    char buf[16] = {};
    vsock::Addr from;
    std::thread t([&] { got = b.recvfrom(fb, buf, sizeof buf, 0, &from); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(send_str(a, fa, kIpB, "wake").ret, 4);
    t.join();
    CHECK_EQ(got.ret, 4);
    CHECK(std::memcmp(buf, "wake", 4) == 0);
    CHECK_EQ(from.ip, kIpA);
    // close() from another thread ends a blocked receive with EBADF.
    std::thread t2([&] { got = b.recvfrom(fb, buf, sizeof buf, 0, &from); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(b.close(fb).ret, 0);
    t2.join();
    CHECK_EQ(got.err, vsock::kEbadf);
}
