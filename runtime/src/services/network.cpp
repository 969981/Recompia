// SPDX-License-Identifier: GPL-3.0-or-later
// Offline network services (S3c): nifm, bsd, bsdcfg, ssl, nim:eca, friend, prepo, pctl, bcat, ldn, nsd, sfdnsres, ntc, ngct, ngc, notif.
// The console has no network connection, no friends and no BCAT data. Every command answers what a console in that state gives. The one
// exceptions are ldn (local wireless over the relay server: services/ldn.cpp registers ldn:u's objects, this file the name) and bsd's UDP
// sockets, which run on the virtual local-wireless LAN (net/vsock.h) when ldn has a vnet link up, or on pokopia-native's co-op module
// ([net] protocol = native: descriptors 0x100..0x11F, coop_vnet_*).
// Adapted from ScarletNX (GPL-3.0-or-later): src/nx/hle/services/network/nifm.swipc, bsd.swipc, ssl.swipc, dns.swipc, ldn.swipc and
//   src/nx/hle/services/misc.swipc (pctl, friend, prepo, bcat, ngct, nsd). Command ids, names and layouts.
// Adapted from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Nifm/*, Sockets/Bsd/IClient.cs, Ssl/ResultCode.cs, Nim/Ntc/*,
//   Notification/*, Ldn/UserServiceCreator/IUserLocalCommunicationService.cs, Pctl/*, Ngct/*, Time/ResultCode.cs. The offline answers
//   and the result codes.
// Switchbrew pages followed for the command tables: Network_Interface_services, Sockets_services, SSL_services, Friend_services,
//   Parental_Control_services. The result codes are module | (description << 9) (services.h).
//
// Each command that is answered logs once at Info with tag "net" (the first use per command). Process-wide state (client ids, the ssl
// context count, the pki ids) lives in st(); network_reset() puts it back to the start-up value (and closes every socket, drops the
// current vnet link and the ldn relay connection). Per-datagram traffic is logged at Debug with tag "net" (vsock.cpp).
#include <memory>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"
#include "ldn.h"
#include "network_internal.h"

namespace nxrt::services {
using namespace netsvc;

// =============================================================================================================================================
// Registration and reset.
// =============================================================================================================================================

void register_network() {
    const bool offline = config_get_bool("net", "offline", true);
    NXLOG_I("net", "network services: [net] offline=%s (no connection, no friends, no BCAT data in either mode)", offline ? "true" : "false");

    if (online_test()) NXLOG_I("net", "[net] online_test = on: nifm and ntc report online, delivery cache empty, no sockets");

    for (const char* n : {"nifm:u", "nifm:a", "nifm:s"}) add(n, [] { return make_nifm_static(); });
    for (const char* n : {"bsd:u", "bsd:a", "bsd:s"}) add(n, [] { return make_bsd(); });
    add("bsdcfg", [] { return make_iface("nn::bsdsocket::cfg::ServerInterface", [](ipc::Service&) {}); });
    // ssl:s (Switch 2; the game opens it, not ssl) is the same ISslService: CreateContext must hand out a real context object.
    for (const char* n : {"ssl", "ssl:s"}) add(n, [] { return make_ssl_service(); });
    add("nim:eca", [] { return make_nim_eca(); });
    for (const char* n : {"friend:u", "friend:v", "friend:m", "friend:s", "friend:a"}) add(n, [] { return make_friend_creator(); });
    for (const char* n : {"prepo:u", "prepo:a", "prepo:a2"}) add(n, [] { return make_prepo(); });
    for (const char* n : {"pctl", "pctl:s", "pctl:r", "pctl:a"}) add(n, [] { return make_pctl_factory(); });
    for (const char* n : {"bcat:u", "bcat:a"}) add(n, [] { return make_bcat_creator(); });
    add("ldn:u", [] { return make_ldn_user_creator(); });
    for (const char* n : {"nsd:u", "nsd:a"}) add(n, [] { return make_nsd(); });
    add("sfdnsres", [] { return make_resolver(); });
    add("ntc", [] { return make_ntc_static(); });
    add("ngct:u", [] { return make_ngct(); });
    add("ngc:u", [] { return make_ngc(); });
    add("notif:a", [] { return make_notif(); });
}

void network_reset() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.epoch.fetch_add(1);   // every command logs its first use again
    s.calls.clear();
    s.next_client_id = 1;
    s.next_cancel_handle = 1;
    s.next_pki_id = 1;
    s.ssl_contexts = 0;
    ldn_reset();   // drops the relay connection (services/ldn.cpp)
    vnet::set_current_link(nullptr);
    vsock::stack().reset();
}

}  // namespace nxrt::services
