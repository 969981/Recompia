// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the offline network services (network*.cpp): result codes, the process state, the command-table helpers (Row, cmd, rows) and
// the service factories register_network() registers.
#pragma once
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

// ---- result codes ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kNifmNoConnection = res(110, 300);         // nifm NoInternetConnection (Ryujinx Nifm/ResultCode.cs)
constexpr uint32_t kTimeNetworkNotAvailable = res(116, 1000); // time NetworkTimeNotAvailable (Ryujinx Time/ResultCode.cs)
constexpr uint32_t kTimeTaskCanceled = res(116, 1003);        // time NetworkTimeTaskCanceled
constexpr uint32_t kSslInvalidSocket = res(123, 106);         // ssl InvalidSocket (Ryujinx Ssl/ResultCode.cs)
constexpr uint32_t kSslInvalidOption = res(123, 126);         // ssl InvalidOption
constexpr uint32_t kFsPathNotFound = res(2, 1002);            // fs PathNotFound 2002: the bcat delivery cache is empty
constexpr uint32_t kBsdNetDown = 100;                         // ENETDOWN (Linux errno numbers, as Switchbrew's bsd page says)
constexpr uint32_t kNetdbHostNotFound = 1;                    // HOST_NOT_FOUND
constexpr int32_t kEaiNoName = -2;                            // EAI_NONAME

// ---- process state -----------------------------------------------------------------------------------------------------------------------
struct NetState {
    std::mutex mu;
    std::set<std::string> seen;          // "<service> <cmd>" already logged
    std::map<std::string, int> calls;    // "<service> <cmd> <name>" -> calls logged so far (at most kMaxCallLogs)
    uint32_t next_client_id = 1;         // nifm ClientId, one per IGeneralService
    uint32_t next_cancel_handle = 1;     // sfdnsres cancel handle
    uint64_t next_pki_id = 1;            // ssl Import*/Register* ids
    int ssl_contexts = 0;                // live ISslContext objects
};

// ---- command tables -----------------------------------------------------------------------------------------------------------------------
// The common answer shapes. Rows are the commands whose answer is fixed (no state, no object, no event).
enum class K : uint8_t {
    Ok,       // result 0, no output (accepted and ignored)
    U8,       // result 0, one u8 = v
    U32,      // result 0, one u32 = v
    S32,      // result 0, one s32 = v
    U64,      // result 0, one u64 = v
    Fail,     // result v (no network, no such object ...)
    Zeros,    // result 0, n zero bytes of output
    BufZero,  // result 0, n zero bytes written into the output buffer
    Errno,    // BSD: (i32 ret = -1, u32 errno = ENETDOWN, then n u32 zeros)
    ErrnoOk,  // BSD: (i32 ret = 0, u32 errno = 0)
    Bytes,    // result 0, the n low bytes of v, lowest first
};
struct Row {
    uint32_t id;
    const char* name;
    K k;
    uint64_t v = 0;   // U8/U32/S32/U64 value, Fail result
    uint32_t n = 0;   // Zeros/BufZero byte count, Errno: extra u32 zeros
};

// Event pairs held by the object that hands them out.
struct TwoEvents {
    ServiceEvent a;
    ServiceEvent b;
};

NetState& st();

// Logs the first use of a command. One line per (service, command) for the life of the process (network_reset() clears it).
void note(const char* svc, uint32_t id, const char* name);
// [net] online_test, read once: nifm and ntc report online (nothing reaches the network: bsd and BCAT answer as before).
bool online_test();

// "ask:" lines for what the game tries to reach (network_common.cpp).
std::string buf_text(const ipc::Buffer& b);
void ask_dns(const ipc::Request& req);
void ask_connect(const vsock::Addr& a);
void ask_ssl_host(const ipc::Request& req);

uint32_t alloc_client_id();
uint64_t alloc_pki_id();
uint32_t alloc_cancel_handle();

// One command with its own handler; logs the first use.
void cmd(ipc::Service& s, const char* svc, uint32_t id, const char* name, ipc::Handler h);
// Every row of a table becomes a command.
void rows(ipc::Service& s, const char* svc, std::initializer_list<Row> list);
// A service object whose release is observed (ssl context and connection counts). on_close runs when the last reference goes.
std::shared_ptr<ipc::Service> tracked(const char* name, std::function<void()> released, const std::function<void(ipc::Service&)>& setup);
// Copy handles belong to the client (see network_common.cpp).
void close_copied(const ipc::Request&);

// Service factories, one per network_*.cpp; register_network() (network.cpp) registers them.
std::shared_ptr<ipc::Service> make_nifm_static();
std::shared_ptr<ipc::Service> make_bsd();
std::shared_ptr<ipc::Service> make_ssl_service();
std::shared_ptr<ipc::Service> make_nim_eca();
std::shared_ptr<ipc::Service> make_friend_creator();
std::shared_ptr<ipc::Service> make_prepo();
std::shared_ptr<ipc::Service> make_pctl_factory();
std::shared_ptr<ipc::Service> make_bcat_creator();
std::shared_ptr<ipc::Service> make_nsd();
std::shared_ptr<ipc::Service> make_resolver();
std::shared_ptr<ipc::Service> make_ntc_static();
std::shared_ptr<ipc::Service> make_ngct();
std::shared_ptr<ipc::Service> make_ngc();
std::shared_ptr<ipc::Service> make_notif();

}  // namespace nxrt::services::netsvc
