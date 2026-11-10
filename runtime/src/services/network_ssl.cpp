// SPDX-License-Identifier: GPL-3.0-or-later
// ssl (ssl, ssl:s) and nim:eca: offline TLS and shop services. See network.cpp for the shared provenance and rules.
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
// ssl: ssl (ISslService). Switchbrew "SSL services". CreateContext answers. Connection calls fail with ssl InvalidSocket: there is no
// socket to run TLS over. Local settings (options, session cache mode, ALPN, verify option) are accepted.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns);

std::shared_ptr<ipc::Service> make_ssl_context() {
    {
        NetState& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        s.ssl_contexts++;
    }
    auto conns = std::make_shared<int>(0);     // live connections of this context
    return tracked(
        "nn::ssl::sf::ISslContext",
        [] {
            NetState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.ssl_contexts > 0) s.ssl_contexts--;
        },
        [conns](ipc::Service& svc) {
            rows(svc, "ssl", {
                {0, "SetOption", K::Ok},
                {1, "GetOption", K::S32, 0},
                {6, "RemoveServerPki", K::Ok},
                {7, "RemoveClientPki", K::Ok},
                {9, "AddPolicyOid", K::Ok},
                {11, "RemoveCrl", K::Ok},
                {13, "GeneratePrivateKeyAndCert", K::Fail, kSslInvalidOption},
            });
            cmd(svc, "ssl", 2, "CreateConnection", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push_object(make_ssl_connection(conns));
                return 0;
            });
            cmd(svc, "ssl", 3, "GetConnectionCount", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push(static_cast<uint32_t>(*conns));
                return 0;
            });
            for (uint32_t id : {4u, 5u, 8u, 10u, 12u}) {
                const char* name = id == 4 ? "ImportServerPki" : id == 5 ? "ImportClientPki" : id == 8 ? "RegisterInternalPki"
                                 : id == 10 ? "ImportCrl" : "ImportClientCertKeyPki";
                cmd(svc, "ssl", id, name, [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                    rsp.push(alloc_pki_id());
                    return 0;
                });
            }
        });
}

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns) {
    ++(*conns);
    std::shared_ptr<int> c = conns;
    return tracked(
        "nn::ssl::sf::ISslConnection",
        [c] { --(*c); },
        [](ipc::Service& svc) {
            cmd(svc, "ssl", 1, "SetHostName", [](ipc::Request& req, ipc::Response&) -> uint32_t {
                ask_ssl_host(req);
                return 0;
            });
            rows(svc, "ssl", {
                {0, "SetSocketDescriptor", K::Fail, kSslInvalidSocket},
                {2, "SetVerifyOption", K::Ok},
                {3, "SetIoMode", K::Ok},
                {4, "GetSocketDescriptor", K::S32, 0xFFFFFFFFu},   // -1: none was set
                {5, "GetHostName", K::U32, 0},
                {6, "GetVerifyOption", K::U32, 0},
                {7, "GetIoMode", K::U32, 0},
                {8, "DoHandshake", K::Fail, kSslInvalidSocket},
                {9, "DoHandshakeGetServerCert", K::Fail, kSslInvalidSocket},
                {10, "Read", K::Fail, kSslInvalidSocket},
                {11, "Write", K::Fail, kSslInvalidSocket},
                {12, "Pending", K::S32, 0},
                {13, "Peek", K::Fail, kSslInvalidSocket},
                {14, "Poll", K::U32, 0},
                {15, "GetVerifyCertError", K::U32, 0},
                {16, "GetNeededServerCertBufferSize", K::U32, 0},
                {17, "SetSessionCacheMode", K::Ok},
                {18, "GetSessionCacheMode", K::U32, 0},
                {19, "FlushSessionCache", K::Ok},
                {20, "SetRenegotiationMode", K::Ok},
                {21, "GetRenegotiationMode", K::U32, 0},
                {22, "SetOption", K::Ok},
                {23, "GetOption", K::U8, 0},
                {24, "GetVerifyCertErrors", K::Ok},
                {25, "GetCipherInfo", K::Fail, kSslInvalidSocket},
                {26, "SetNextAlpnProto", K::Ok},
                {27, "GetNextAlpnProto", K::Zeros, 0, 8},          // (u32 state, u32 size) = (0, 0)
                {28, "SetDtlsSocketDescriptor", K::Fail, kSslInvalidSocket},
                {29, "GetDtlsHandshakeTimeout", K::U64, 0},
                {30, "SetPrivateOption", K::Ok},
                {31, "SetSrtpCiphers", K::Ok},
                {32, "GetSrtpCipher", K::U32, 0},
                {33, "ExportKeyingMaterial", K::Fail, kSslInvalidSocket},
                {34, "SetIoTimeout", K::Ok},
                {35, "GetIoTimeout", K::U64, 0},
                {36, "GetSessionTicket", K::U32, 0},
                {37, "SetSessionTicket", K::Ok},
            });
        });
}

}  // namespace

std::shared_ptr<ipc::Service> make_ssl_service() {
    return make_iface("nn::ssl::sf::ISslService", [](ipc::Service& s) {
        cmd(s, "ssl", 0, "CreateContext", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ssl_context());
            return 0;
        });
        cmd(s, "ssl", 1, "GetContextCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push(static_cast<uint32_t>(n.ssl_contexts));
            return 0;
        });
        rows(s, "ssl", {
            {2, "GetCertificates", K::U32, 0},
            {3, "GetCertificateBufSize", K::U32, 0},
            {4, "DebugIoctl", K::Fail, kSslInvalidOption},
            {5, "SetInterfaceVersion", K::Ok},
            {6, "FlushSessionCache", K::U32, 0},
            {7, "SetDebugOption", K::Ok},
            {8, "GetDebugOption", K::BufZero, 0, 1},
            {9, "ClearTls12FallbackFlag", K::Ok},
            {10, "GetCertificateByIndex", K::Ok},
            {11, "GetTrustedCertificateCount", K::U32, 0},
        });
    });
}

namespace {

// =============================================================================================================================================
// nim:eca: the shop (ECA) interface. Creation succeeds; the shop objects answer their creation commands, nothing needs the network.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_shop_async() {
    return make_iface("nn::ec::IShopServiceAsync", [](ipc::Service&) {});   // no command table is read yet: any command is unhandled
}

std::shared_ptr<ipc::Service> make_shop_accessor() {
    return make_iface("nn::ec::IShopServiceAccessor", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAsyncInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_async());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_shop_server() {
    return make_iface("nn::ec::IShopServiceAccessServer", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAccessorInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_accessor());
            return 0;
        });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_nim_eca() {
    return make_iface("nn::nim::detail::IShopServiceAccessServerInterface", [](ipc::Service& s) {
        auto create = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);
            rsp.push_object(make_shop_server());
            return 0;
        };
        cmd(s, "nim:eca", 0, "CreateServerInterface", create);
        rows(s, "nim:eca", {
            {4, "IsLargeResourceAvailable", K::U8, 0},
        });
        cmd(s, "nim:eca", 5, "CreateServerInterface2", create);
    });
}

}  // namespace nxrt::services::netsvc
