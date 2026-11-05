// SPDX-License-Identifier: GPL-3.0-or-later
// Offline network services (S3c): what a console without a connection, friends or BCAT data answers. The request bytes and the
// expected values are written out from Switchbrew's command ids and result codes (result = module | (description << 9)), not read from
// the service under test.
#include "../src/net/vnet.h"
#include "../src/net/vsock.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_network)

using namespace nxrt;

namespace {
constexpr uint32_t kNifm300 = 0x2586E;          // nifm 300 NoInternetConnection: 110 | (300 << 9)
constexpr uint32_t kLdn65 = 0x82CB;             // ldn 65 ConnectNotFound: 203 | (65 << 9)
constexpr uint32_t kNtc1000 = 0x7D074;          // time 1000 NetworkTimeNotAvailable: 116 | (1000 << 9)
constexpr uint32_t kSslInvalidSocket = 0xD47B;  // ssl 106 InvalidSocket: 123 | (106 << 9)

// Waits (no timeout) on a copied event handle: true when it is signalled.
bool signalled(uint32_t h) {
    int32_t idx = -1;
    return kernel::wait_synchronization(&h, 1, 0, &idx) == 0;
}
}  // namespace

NXTEST(nifm_no_connection) {
    nxsvc::Env env;
    nxsvc::Session nifm = nxsvc::connect("nifm:u");
    REQUIRE(nifm.valid());
    // CreateGeneralService(pid, u64 reserved) -> IGeneralService (command 5)
    nxsvc::Reply g = nifm.call(5, nxsvc::Args().with_pid().u64(0));
    REQUIRE(g.ok());
    nxsvc::Session gen = nifm.sub(g);
    REQUIRE(gen.valid());
    // GetCurrentIpAddress (12): no address without a connection
    CHECK_EQ(gen.call(12).result, kNifm300);
    // IsAnyForegroundRequestAccepted (22): false, plain u8 output
    nxsvc::Reply fg = gen.call(22);
    CHECK_EQ(fg.result, 0u);
    CHECK_EQ(fg.get<uint8_t>(0), 0);
    // CreateRequest(s32 preset = 2) (4) -> IRequest; GetRequestState (0) is 1 (Free); GetResult (1) is the no-connection result
    nxsvc::Session req = gen.sub(gen.call(4, nxsvc::Args().i32(2)));
    REQUIRE(req.valid());
    CHECK_EQ(req.call(0).get<uint32_t>(0), 1u);
    CHECK_EQ(req.call(1).result, kNifm300);
    // GetSystemEventReadableHandles (2): two copies of events that never signal
    nxsvc::Reply ev = req.call(2);
    REQUIRE(ev.copy.size() == 2);
    CHECK(!signalled(ev.copy[0]));
    CHECK(!signalled(ev.copy[1]));
    for (uint32_t h : ev.copy) kernel::handle_close(h);
}

NXTEST(bsd_socket_enetdown) {
    nxsvc::Env env;
    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    // Socket(i32 domain = AF_INET 2, i32 type = SOCK_STREAM 1, i32 protocol 0) -> (i32 ret, u32 errno): -1, ENETDOWN 100
    nxsvc::Reply r = bsd.call(2, nxsvc::Args().i32(2).i32(1).i32(0));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), -1);
    CHECK_EQ(r.get<uint32_t>(4), 100u);
    // Close(i32 fd) -> (i32 ret, u32 errno): no such socket, -1 EBADF 9
    nxsvc::Reply c = bsd.call(26, nxsvc::Args().i32(3));
    CHECK_EQ(c.result, 0u);
    CHECK_EQ(c.get<int32_t>(0), -1);
    CHECK_EQ(c.get<uint32_t>(4), 9u);
    // Listen (18), a TCP command, stays offline: -1, ENETDOWN
    CHECK_EQ(bsd.call(18, nxsvc::Args().i32(0).i32(1)).get<uint32_t>(4), 100u);
}

// The local-wireless transport's exact sequence (Pokopia CNnLdnOnlineSessionP2p, static recon): Socket(AF_INET, SOCK_DGRAM |
// SOCK_NONBLOCK), SetSockOpt(SOL_SOCKET, SO_BROADCAST), Bind 0.0.0.0:49530, SendTo a peer, RecvFrom with MSG_DONTWAIT. The node's link
// is the current vnet link; the peer is a second link on the same in-process hub with its own socket table.
NXTEST(bsd_udp_game_sequence) {
    nxsvc::Env env;
    vnet::LoopbackHub hub;
    const uint32_t me = vnet::ip(169, 254, 1, 1), peer_ip = vnet::ip(169, 254, 1, 2);
    vnet::set_current_link(hub.add(me));
    vsock::Stack peer(hub.add(peer_ip));
    const int pfd = peer.socket(2, 2, 0).ret;
    REQUIRE(pfd >= 0);
    REQUIRE(peer.bind(pfd, {0, 49530}).ret == 0);

    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    // Socket(i32 2, i32 0x20000002, i32 0) -> (i32 fd, u32 errno 0)
    nxsvc::Reply s = bsd.call(2, nxsvc::Args().i32(2).i32(0x20000002).i32(0));
    CHECK_EQ(s.result, 0u);
    const int32_t fd = s.get<int32_t>(0);
    CHECK(fd >= 0);
    CHECK_EQ(s.get<uint32_t>(4), 0u);
    // SetSockOpt(i32 fd, i32 SOL_SOCKET 0xffff, i32 SO_BROADCAST 0x20, buffer 0x21 = int 1) -> (0, 0)
    const int32_t one = 1;
    nxsvc::Reply so = bsd.call(21, nxsvc::Args().i32(fd).i32(0xFFFF).i32(0x20).buf_a(&one, 4));
    CHECK_EQ(so.get<int32_t>(0), 0);
    CHECK_EQ(so.get<uint32_t>(4), 0u);
    // Bind(i32 fd, buffer 0x21 = sockaddr_in {len 16, AF_INET, be16 49530 (0xC17A), 0.0.0.0}) -> (0, 0)
    const uint8_t any[16] = {16, 2, 0xC1, 0x7A, 0, 0, 0, 0};
    nxsvc::Reply b = bsd.call(13, nxsvc::Args().i32(fd).buf_a(any, 16));
    CHECK_EQ(b.get<int32_t>(0), 0);
    CHECK_EQ(b.get<uint32_t>(4), 0u);
    // SendTo(i32 fd, i32 flags 0, buffer 0x21 data, buffer 0x21 sockaddr 169.254.1.2:49530) -> (bytes sent, 0)
    const char ping[] = "\x01\x02\x03\x04ping";
    const uint8_t to[16] = {16, 2, 0xC1, 0x7A, 169, 254, 1, 2};
    nxsvc::Reply st = bsd.call(11, nxsvc::Args().i32(fd).i32(0).buf_a(ping, 8).buf_a(to, 16));
    CHECK_EQ(st.get<int32_t>(0), 8);
    CHECK_EQ(st.get<uint32_t>(4), 0u);
    char pbuf[16] = {};
    vsock::Addr from;
    CHECK_EQ(peer.recvfrom(pfd, pbuf, sizeof pbuf, vsock::kMsgDontwait, &from).ret, 8);
    CHECK(std::memcmp(pbuf, ping, 8) == 0);
    CHECK_EQ(from.ip, me);
    CHECK_EQ(from.port, uint16_t{49530});

    // RecvFrom(i32 fd, i32 MSG_DONTWAIT 0x80, buffer 0x22 data 0x1000, buffer 0x22 sockaddr) -> (i32 len, u32 errno, u32 addrlen)
    std::vector<uint8_t> rbuf(0x1000);
    uint8_t sa[16] = {};
    nxsvc::Reply empty = bsd.call(9, nxsvc::Args().i32(fd).i32(0x80).buf_b(rbuf.data(), 0x1000).buf_b(sa, 16));
    CHECK_EQ(empty.get<int32_t>(0), -1);
    CHECK_EQ(empty.get<uint32_t>(4), 11u);   // EAGAIN
    const vsock::Addr back{me, 49530};
    CHECK_EQ(peer.sendto(pfd, "pong", 4, 0, &back).ret, 4);
    nxsvc::Reply rf = bsd.call(9, nxsvc::Args().i32(fd).i32(0x80).buf_b(rbuf.data(), 0x1000).buf_b(sa, 16));
    CHECK_EQ(rf.get<int32_t>(0), 4);
    CHECK_EQ(rf.get<uint32_t>(4), 0u);
    CHECK_EQ(rf.get<uint32_t>(8), 16u);
    CHECK(std::memcmp(rbuf.data(), "pong", 4) == 0);
    const uint8_t want[8] = {16, 2, 0xC1, 0x7A, 169, 254, 1, 2};
    CHECK(std::memcmp(sa, want, 8) == 0);
    // The peer's subnet broadcast reaches the game's socket.
    int32_t on = 1;
    peer.setsockopt(pfd, vsock::kSolSocket, vsock::kSoBroadcast, &on, 4);
    const vsock::Addr all{vnet::ip(169, 254, 1, 255), 49530};
    CHECK_EQ(peer.sendto(pfd, "all", 3, 0, &all).ret, 3);
    CHECK_EQ(bsd.call(9, nxsvc::Args().i32(fd).i32(0x80).buf_b(rbuf.data(), 0x1000).buf_b(sa, 16)).get<int32_t>(0), 3);
    // GetSockName(i32 fd, buffer 0x22) -> (0, 0, addrlen 16): 0.0.0.0:49530
    uint8_t name[16] = {};
    nxsvc::Reply gn = bsd.call(16, nxsvc::Args().i32(fd).buf_b(name, 16));
    CHECK_EQ(gn.get<int32_t>(0), 0);
    CHECK_EQ(gn.get<uint32_t>(8), 16u);
    CHECK(std::memcmp(name, any, 8) == 0);
    // Fcntl(fd, F_GETFL 3, 0): O_NONBLOCK 0x800 from SOCK_NONBLOCK
    CHECK_EQ(bsd.call(20, nxsvc::Args().i32(fd).i32(3).i32(0)).get<int32_t>(0), 0x800);
    // Close -> (0, 0); the link goes down -> SendTo on a new socket is ENETUNREACH 101
    nxsvc::Reply c = bsd.call(26, nxsvc::Args().i32(fd));
    CHECK_EQ(c.get<int32_t>(0), 0);
    CHECK_EQ(c.get<uint32_t>(4), 0u);
    vnet::set_current_link(nullptr);
    const int32_t fd2 = bsd.call(2, nxsvc::Args().i32(2).i32(2).i32(0)).get<int32_t>(0);
    nxsvc::Reply un = bsd.call(11, nxsvc::Args().i32(fd2).i32(0).buf_a(ping, 8).buf_a(to, 16));
    CHECK_EQ(un.get<int32_t>(0), -1);
    CHECK_EQ(un.get<uint32_t>(4), 101u);
}

NXTEST(ssl_context_connection_fails) {
    nxsvc::Env env;
    nxsvc::Session ssl = nxsvc::connect("ssl");
    REQUIRE(ssl.valid());
    // CreateContext(u32 SslVersion, pad, u64 pid placeholder) with the pid -> ISslContext (command 0)
    nxsvc::Session ctx = ssl.sub(ssl.call(0, nxsvc::Args().with_pid().u32(5).pad(4).u64(0)));
    REQUIRE(ctx.valid());
    CHECK_EQ(ctx.call(3).get<uint32_t>(0), 0u);     // GetConnectionCount: none yet
    nxsvc::Session conn = ctx.sub(ctx.call(2));     // CreateConnection (2) -> ISslConnection
    REQUIRE(conn.valid());
    CHECK_EQ(ctx.call(3).get<uint32_t>(0), 1u);
    CHECK_EQ(conn.call(8).result, kSslInvalidSocket);   // DoHandshake: no socket
    CHECK_EQ(conn.call(0, nxsvc::Args().i32(3)).result, kSslInvalidSocket);   // SetSocketDescriptor
    // Closing the connection drops the count again (on_close of the released object)
    conn.close();
    CHECK_EQ(ctx.call(3).get<uint32_t>(0), 0u);
}

NXTEST(nim_eca_creation_succeeds) {
    nxsvc::Env env;
    nxsvc::Session nim = nxsvc::connect("nim:eca");
    REQUIRE(nim.valid());
    services::ServiceEvent stand_in;   // the transfer memory argument: a copy handle the server closes
    const uint32_t h = stand_in.new_handle();
    REQUIRE(h != 0);
    // CreateServerInterface(pid, handle<copy>, u64) (0) -> IShopServiceAccessServer
    nxsvc::Reply r = nim.call(0, nxsvc::Args().with_pid().hcopy(h).u64(0));
    CHECK_EQ(r.result, 0u);
    nxsvc::Session server = nim.sub(r);
    REQUIRE(server.valid());
    nxsvc::Session accessor = server.sub(server.call(0));   // CreateAccessorInterface (0)
    REQUIRE(accessor.valid());
    CHECK_EQ(accessor.call(0).result, 0u);                   // CreateAsyncInterface (0)
    CHECK_EQ(nim.call(4).get<uint8_t>(0), 0);                 // IsLargeResourceAvailable: false
}

NXTEST(friend_empty_list) {
    nxsvc::Env env;
    nxsvc::Session fr = nxsvc::connect("friend:u");
    REQUIRE(fr.valid());
    nxsvc::Session fs = fr.sub(fr.call(0));                  // CreateFriendService (0)
    REQUIRE(fs.valid());
    CHECK_EQ(fs.call(20100).get<int32_t>(0), 0);             // GetFriendCount: 0
    nxsvc::Reply list = fs.call(10101, nxsvc::Args().i32(0));   // GetFriendList: count 0
    CHECK_EQ(list.result, 0u);
    CHECK_EQ(list.get<int32_t>(0), 0);
    nxsvc::Reply done = fs.call(0);                          // GetCompletionEvent: already signalled
    REQUIRE(done.copy.size() == 1);
    CHECK(signalled(done.copy[0]));
    kernel::handle_close(done.copy[0]);
}

NXTEST(pctl_no_restriction) {
    nxsvc::Env env;
    nxsvc::Session pc = nxsvc::connect("pctl");
    REQUIRE(pc.valid());
    nxsvc::Session s = pc.sub(pc.call(0, nxsvc::Args().with_pid()));   // CreateService(pid) (0)
    REQUIRE(s.valid());
    CHECK_EQ(s.call(1031).get<uint8_t>(0), 0);                // IsRestrictionEnabled: false
    CHECK_EQ(s.call(1018).get<uint8_t>(0), 1);                // IsFreeCommunicationAvailable: true
    CHECK_EQ(s.call(1065).get<uint8_t>(0), 1);                // IsStereoVisionPermitted: true
    CHECK_EQ(s.call(1039).get<int32_t>(0), 0);                // GetFreeCommunicationApplicationListCount: 0
    CHECK_EQ(s.call(1001).result, 0u);                        // CheckFreeCommunicationPermission: allowed
}

NXTEST(ldn_state_machine_never_finds_a_network) {
    nxsvc::Env env;
    nxsvc::Session ldn = nxsvc::connect("ldn:u");
    REQUIRE(ldn.valid());
    nxsvc::Session l = ldn.sub(ldn.call(0));                  // CreateUserLocalCommunicationService (0)
    REQUIRE(l.valid());
    CHECK_EQ(l.call(0).get<uint32_t>(0), 0u);                  // GetState: None before Initialize
    CHECK_EQ(l.call(400, nxsvc::Args().with_pid()).result, 0u);   // Initialize(pid) (400) is accepted
    CHECK_EQ(l.call(0).get<uint32_t>(0), 1u);                  // now Initialized
    CHECK_EQ(l.call(102, nxsvc::Args().u16(0)).get<uint16_t>(0), 0);   // Scan: count 0
    CHECK_EQ(l.call(302, nxsvc::Args()).result, kLdn65);       // Connect: ConnectNotFound
    CHECK_EQ(l.call(401).result, 0u);                          // Finalize
    CHECK_EQ(l.call(0).get<uint32_t>(0), 0u);                  // back to None
}

NXTEST(ntc_network_clock_task_fails) {
    nxsvc::Env env;
    nxsvc::Session ntc = nxsvc::connect("ntc");
    REQUIRE(ntc.valid());
    nxsvc::Session t = ntc.sub(ntc.call(0));                  // CreateEnsureNetworkClockAvailabilityService (0)
    REQUIRE(t.valid());
    CHECK_EQ(t.call(0).result, kNtc1000);                     // StartTask: network not available
    CHECK_EQ(t.call(2).result, kNtc1000);                     // GetResult: the same
    nxsvc::Reply fin = t.call(1);                             // GetFinishNotificationEvent: signalled when the task ends
    REQUIRE(fin.copy.size() == 1);
    CHECK(signalled(fin.copy[0]));
    kernel::handle_close(fin.copy[0]);
}

NXTEST(bcat_sync_done_with_empty_cache) {
    nxsvc::Env env;
    nxsvc::Session bcat = nxsvc::connect("bcat:u");
    REQUIRE(bcat.valid());
    nxsvc::Session b = bcat.sub(bcat.call(0, nxsvc::Args().with_pid()));   // CreateBcatService(pid) (0)
    REQUIRE(b.valid());
    nxsvc::Session prog = b.sub(b.call(10100));               // RequestSyncDeliveryCache (10100)
    REQUIRE(prog.valid());
    nxsvc::Reply ev = prog.call(0);                           // GetEvent (0): the sync is over at once
    REQUIRE(ev.copy.size() == 1);
    CHECK(signalled(ev.copy[0]));
    kernel::handle_close(ev.copy[0]);
    // Open a file in the empty cache: path not found (fs 2002)
    nxsvc::Session storage = bcat.sub(bcat.call(1, nxsvc::Args().with_pid()));   // CreateDeliveryCacheStorageService (1)
    REQUIRE(storage.valid());
    nxsvc::Session file = storage.sub(storage.call(0));       // CreateFileService (0)
    REQUIRE(file.valid());
    CHECK_EQ(file.call(0).result, 0x7D402u);                  // Open: 2 | (1002 << 9) = PathNotFound (fs 2002)
}

NXTEST(nsd_environment_and_resolve) {
    nxsvc::Env env;
    nxsvc::Session nsd = nxsvc::connect("nsd:u");
    REQUIRE(nsd.valid());
    char env_id[8] = {};
    CHECK_EQ(nsd.call(11, nxsvc::Args().buf_c(env_id, 8)).result, 0u);   // GetEnvironmentIdentifier: "lp1"
    CHECK(std::memcmp(env_id, "lp1", 3) == 0);
    char name[0x100] = "example.invalid";
    char out[0x100] = {};
    CHECK_EQ(nsd.call(20, nxsvc::Args().buf_x(name, 0x100).buf_c(out, 0x100)).result, 0u);   // Resolve: the name as it was
    CHECK(std::memcmp(out, "example.invalid", 16) == 0);
}

NXTEST(ngct_returns_text_unchanged) {
    nxsvc::Env env;
    nxsvc::Session ngct = nxsvc::connect("ngct:u");
    REQUIRE(ngct.valid());
    char text[] = "hello";
    CHECK_EQ(ngct.call(0, nxsvc::Args().buf_a(text, 5)).get<uint8_t>(0), 0);   // Match: nothing found
    char out[8] = {};
    CHECK_EQ(ngct.call(1, nxsvc::Args().buf_a(text, 5).buf_c(out, 5)).result, 0u);   // Filter: the text back
    CHECK(std::memcmp(out, "hello", 5) == 0);
}

// Pokopia's name check calls ngc:u Mask (text in buffer A, masked text in buffer B) and refuses the name if it fails (fork issue #6).
NXTEST(ngc_masks_nothing) {
    nxsvc::Env env;
    nxsvc::Session ngc = nxsvc::connect("ngc:u");
    REQUIRE(ngc.valid());
    char text[] = "Alex";
    CHECK_EQ(ngc.call(1, nxsvc::Args().buf_a(text, 5)).get<uint32_t>(0), 0u);   // Check: nothing matched
    char out[8] = {};
    nxsvc::Reply r = ngc.call(2, nxsvc::Args().buf_a(text, 5).buf_b(out, sizeof out));
    CHECK_EQ(r.result, 0u);                                                     // Mask: success,
    CHECK_EQ(r.get<uint32_t>(0), 0u);                                           // no word masked,
    CHECK(std::memcmp(out, "Alex", 5) == 0);                                    // the text back
}

NXTEST(prepo_and_notif_accept) {
    nxsvc::Env env;
    nxsvc::Session prepo = nxsvc::connect("prepo:u");
    REQUIRE(prepo.valid());
    CHECK_EQ(prepo.call(10400).get<uint64_t>(0), 0ull);       // GetSystemSessionId: 0
    CHECK_EQ(prepo.call(10200).result, 0u);                   // RequestImmediateTransmission: accepted
    nxsvc::Session notif = nxsvc::connect("notif:a");
    REQUIRE(notif.valid());
    CHECK_EQ(notif.call(520).get<int32_t>(0), 0);             // ListAlarmSettings: count 0
}

NXTEST(sfdnsres_lookups_fail) {
    nxsvc::Env env;
    nxsvc::Session dns = nxsvc::connect("sfdnsres");
    REQUIRE(dns.valid());
    nxsvc::Reply r = dns.call(6, nxsvc::Args());               // GetAddrInfoRequest: (bsd_errno, gai_error, size)
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(4), -2);                           // EAI_NONAME
    CHECK_EQ(dns.call(8, nxsvc::Args().with_pid()).get<uint32_t>(0), 1u);   // GetCancelHandleRequest: first handle
}
