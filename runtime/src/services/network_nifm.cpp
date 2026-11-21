// SPDX-License-Identifier: GPL-3.0-or-later
// nifm (nifm:u, nifm:a, nifm:s): the network interface services, offline. See network.cpp for the shared provenance and rules.
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
#include "network_cloud.h"
#include "services_internal.h"

namespace nxrt::services::netsvc {

namespace {

// =============================================================================================================================================
// nifm: nifm:u, nifm:a, nifm:s (IStaticService). Switchbrew "Network Interface services", ScarletNX nifm.swipc, Ryujinx Nifm/.
// No connection: GetCurrentIpAddress and the other address/profile answers return nifm 300 (NoInternetConnection); when reporting online (report_online())
// they report online instead. A connection request (IRequest) is accepted only with a Cloud Islands server: see make_request().
// =============================================================================================================================================

// Logged once: the game is told it is online (events) and asks for a connection, which has no server to go to.
void note_request_refused() {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        NXLOG_I("net", "nifm: connection request refused: reporting online, but no Cloud Islands server is set ([net] cloud_server); the game shows its communication error");
}

std::shared_ptr<ipc::Service> make_scan_request() {
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("nn::nifm::detail::IScanRequest", [ev](ipc::Service& s) {
        const bool online = report_online();
        cmd(s, "nifm", 0, "Submit", [ev, online](ipc::Request&, ipc::Response&) -> uint32_t {
            if (online) ev->signal();   // a scan that is over at once: nothing may wait on its event
            return 0;
        });
        rows(s, "nifm", {
            {1, "IsProcessing", K::U8, 0},
            {2, "GetResult", K::Ok},            // a scan that is over at once and found nothing
            {4, "SetChannels", K::Ok},
            {5, "Cancel", K::Ok},
        });
        cmd(s, "nifm", 3, "GetSystemEventReadableHandle", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

// A request is accepted only with a Cloud Islands server ([net] cloud_server). Reporting online without one ([pkp] online_events, on by
// default, or [net] online_test) keeps the status answers in make_general() online for events, but a request is answered as offline: an
// accepted one sent Create a new Cloud Island on to NPLN, where bsd EventFd answers ENETDOWN without a server and the SDK aborted
// (svcBreak). Refused, the game handles it as with no connection: HandleNetworkRequestErrorResult, then its communication error (2006).
std::shared_ptr<ipc::Service> make_request() {
    const bool accepted = cloud::enabled();
    const bool refused_online = !accepted && report_online();
    auto ev = std::make_shared<TwoEvents>();
    return make_iface("nn::nifm::detail::IRequest", [ev, accepted, refused_online](ipc::Service& s) {
        rows(s, "nifm", {
            {0, "GetRequestState", K::U32, static_cast<uint64_t>(accepted ? 3 : 1)},    // 1 = Free: nothing was accepted; 3 = Accepted
            {1, "GetResult", accepted ? K::Ok : K::Fail, kNifmNoConnection},
            {5, "SetRequirement", K::Ok},
            {6, "SetRequirementPreset", K::Ok},
            {8, "SetPriority", K::Ok},
            {9, "SetNetworkProfileId", K::Ok},
            {10, "SetRejectable", K::Ok},
            {11, "SetConnectionConfirmationOption", K::Ok},
            {12, "SetPersistent", K::Ok},
            {13, "SetInstant", K::Ok},
            {14, "SetSustainable", K::Ok},
            {15, "SetRawPriority", K::Ok},
            {16, "SetGreedy", K::Ok},
            {17, "SetSharable", K::Ok},
            {18, "SetRequirementByRevision", K::Ok},
            {19, "GetRequirement", K::Zeros, 0, 0x24},           // 0x20 bytes, plus 4 from 3.0.0
            {20, "GetRevision", K::U32, 0},
            {22, "GetAdditionalInfo", K::Ok},
            {23, "SetKeptInSleep", K::Ok},
            {24, "RegisterSocketDescriptor", K::Ok},
            {25, "UnregisterSocketDescriptor", K::Ok},
            {26, "GetNetworkAccessStatus", K::U32, 0},
        });
        // Accepted, Submit and Cancel signal both events (the request is accepted at once), so a wait on them ends.
        for (const auto& c : {std::pair<uint32_t, const char*>{3, "Cancel"}, {4, "Submit"}})
            cmd(s, "nifm", c.first, c.second, [ev, accepted, refused = refused_online && c.first == 4](ipc::Request&, ipc::Response&) -> uint32_t {
                if (accepted) { ev->a.signal(); ev->b.signal(); }
                if (refused) note_request_refused();
                return 0;
            });
        cmd(s, "nifm", 2, "GetSystemEventReadableHandles", [ev, accepted](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            if (accepted) { ev->a.signal(); ev->b.signal(); }   // the state is already Accepted
            rsp.out_copy.push_back(ev->a.new_handle());
            rsp.out_copy.push_back(ev->b.new_handle());
            return 0;
        });
        cmd(s, "nifm", 21, "GetAppletInfo", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // The error applet (0xE) with no data, as Ryujinx answers it when the request has failed.
            rsp.push(static_cast<uint32_t>(0xE));
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_general() {
    const bool online = report_online();
    const uint32_t client_id = alloc_client_id();
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("nn::nifm::detail::IGeneralService", [client_id, ev, online](ipc::Service& s) {
        cmd(s, "nifm", 1, "GetClientId", [client_id](ipc::Request& req, ipc::Response&) -> uint32_t {
            buf_write(req.out_buf(0), &client_id, sizeof client_id);
            return 0;
        });
        cmd(s, "nifm", 2, "CreateScanRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_scan_request());
            return 0;
        });
        cmd(s, "nifm", 4, "CreateRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_request());
            return 0;
        });
        // Online: a profile and an address setting for a private network. Layouts as recalled from switchbrew (the profile 0x17C
        // bytes, zeros but for a uuid and a name; the IP setting 0xD bytes, then the DNS setting): the sizes matter more than the fields.
        cmd(s, "nifm", 5, "GetCurrentNetworkProfile", [online](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!online) return kNifmNoConnection;
            uint8_t prof[0x17C] = {};
            prof[0] = 0x52; prof[1] = 0x45; prof[2] = 0x43; prof[3] = 0x4F;       // uuid bytes
            std::memcpy(prof + 0x10, "Recompia", 8);                              // name
            buf_write(req.out_buf(0), prof, sizeof prof);
            return 0;
        });
        cmd(s, "nifm", 15, "GetCurrentIpConfigInfo", [online](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            if (!online) return kNifmNoConnection;
            // IpAddressSetting: is_auto=1, address 10.0.0.2, mask 255.255.255.0, gateway 10.0.0.1; DnsSetting: is_auto=1, 10.0.0.1, 10.0.0.1
            const uint8_t ip[0xD] = {1, 10, 0, 0, 2, 255, 255, 255, 0, 10, 0, 0, 1};
            const uint8_t dns[0x9] = {1, 10, 0, 0, 1, 10, 0, 0, 1};
            if (req.out_buf(0).addr) {
                buf_write(req.out_buf(0), ip, sizeof ip);
                if (req.out_buf(1).addr) buf_write(req.out_buf(1), dns, sizeof dns);
            } else {
                uint8_t raw[0x1C] = {};
                std::memcpy(raw, ip, sizeof ip);
                std::memcpy(raw + 0xD, dns, sizeof dns);
                for (size_t i = 0; i < sizeof raw; i += 4) { uint32_t w; std::memcpy(&w, raw + i, 4); rsp.push(w); }
            }
            return 0;
        });
        cmd(s, "nifm", 31, "GetTelemetorySystemEventReadableHandle", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        rows(s, "nifm", {
            {6, "EnumerateNetworkInterfaces", K::S32, 0},
            {7, "EnumerateNetworkProfiles", K::S32, 0},
            {8, "GetNetworkProfile", K::Fail, kNifmNoConnection},
            {9, "SetNetworkProfile", K::Zeros, 0, 16},          // returns a Uuid (16 zero bytes); the profile is not kept
            {10, "RemoveNetworkProfile", K::Ok},
            {11, "GetScanDataOld", K::S32, 0},
            {12, "GetCurrentIpAddress", online ? K::Bytes : K::Fail, online ? 0x0200000Aull : kNifmNoConnection, 4},   // online: 10.0.0.2
            {13, "GetCurrentAccessPointOld", K::Fail, kNifmNoConnection},
            {14, "CreateTemporaryNetworkProfile", K::Fail, kNifmNoConnection},
            {16, "SetWirelessCommunicationEnabled", K::Ok},
            {17, "IsWirelessCommunicationEnabled", K::U8, 1},   // radio on, no access point
            {18, "GetInternetConnectionStatus", online ? K::Bytes : K::Fail, online ? 0x040301ull : kNifmNoConnection, 3},   // online: Wifi, strength 3, Connected
            {19, "SetEthernetCommunicationEnabled", K::Ok},
            {20, "IsEthernetCommunicationEnabled", K::U8, 0},   // no adapter
            {21, "IsAnyInternetRequestAccepted", K::U8, static_cast<uint64_t>(online ? 1 : 0)},
            {22, "IsAnyForegroundRequestAccepted", K::U8, static_cast<uint64_t>(online ? 1 : 0)},
            {23, "PutToSleep", K::Ok},
            {24, "WakeUp", K::Ok},
            {25, "GetSsidListVersion", K::U32, 0},
            {26, "SetExclusiveClient", K::Ok},
            {27, "GetDefaultIpSetting", K::Zeros, 0, 0xD},
            {28, "SetDefaultIpSetting", K::Ok},
            {29, "SetWirelessCommunicationEnabledForTest", K::Ok},
            {30, "SetEthernetCommunicationEnabledForTest", K::Ok},
            {32, "GetTelemetryInfo", K::Ok},
            {33, "ConfirmSystemAvailability", online ? K::Ok : K::Fail, kNifmNoConnection},
            {101, "Cmd101", K::Zeros, 0, 16},   // Switch 2, no public name: the SDK calls it when the game goes online and exits if it fails
            {34, "SetBackgroundRequestEnabled", K::Ok},
            {35, "GetScanData", K::S32, 0},
            {36, "GetCurrentAccessPoint", K::Fail, kNifmNoConnection},
            {37, "Shutdown", K::Ok},
            {38, "GetAllowedChannels", K::Ok},
            {39, "NotifyApplicationSuspended", K::Ok},
            {40, "SetAcceptableNetworkTypeFlag", K::Ok},
            {41, "GetAcceptableNetworkTypeFlag", K::U32, 0},
            {42, "NotifyConnectionStateChanged", K::Ok},
            {43, "SetWowlDelayedWakeTime", K::Ok},
            {44, "IsWiredConnectionAvailable", K::U8, 0},
            {45, "IsNetworkEmulationFeatureEnabled", K::U8, 0},
            {46, "SelectActiveNetworkEmulationProfileIdForDebug", K::Ok},
            {47, "GetScanData", K::S32, 0},
            {48, "ResetActiveNetworkEmulationProfileId", K::Ok},
            {49, "GetActiveNetworkEmulationProfileId", K::U32, 0},
            {50, "IsRewriteFeatureEnabled", K::U8, 0},
            {51, "CreateRewriteRule", K::Ok},
            {52, "DestroyRewriteRule", K::Ok},
            {53, "IsActiveNetworkEmulationProfileIdSelected", K::U8, 0},
            {54, "SelectDefaultNetworkEmulationProfileId", K::Ok},
            {55, "GetDefaultNetworkEmulationProfileId", K::U32, 0},
            {56, "GetNetworkEmulationProfile", K::Fail, kNifmNoConnection},
            {57, "SetWowlTcpKeepAliveTimeout", K::Ok},
        });
        if (netsvc::cloud::enabled())   // Cloud Island: the game's peer-to-peer code compares this with its interface list (bsd Sysctl)
            cmd(s, "nifm", 12, "GetCurrentIpAddress", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                const uint32_t ip = netsvc::cloud::local_ip();
                rsp.push(static_cast<uint32_t>((ip >> 24) | ((ip >> 16) & 255) << 8 | ((ip >> 8) & 255) << 16 | (ip & 255) << 24));
                return 0;
            });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_nifm_static() {
    return make_iface("nn::nifm::detail::IStaticService", [](ipc::Service& s) {
        cmd(s, "nifm", 4, "CreateGeneralServiceOld", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_general());
            return 0;
        });
        cmd(s, "nifm", 5, "CreateGeneralService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_general());
            return 0;
        });
    });
}

}  // namespace nxrt::services::netsvc
