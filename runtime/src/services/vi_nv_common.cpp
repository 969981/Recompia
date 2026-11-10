// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the vi and nvdrv services (see vi_nv.cpp).
#include "vi_nv_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services::vinv {

// ============================================================ shared helpers

Arg arg_of(const ipc::Buffer& b) { return Arg{buf_mut(b), b.size}; }

// The NUL-terminated string a guest buffer holds (at most the buffer's size).
std::string c_string(const ipc::Buffer& b) {
    std::string s;
    if (!b.addr) return s;
    const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(b.addr));
    s.assign(p, strnlen(p, static_cast<size_t>(b.size)));
    return s;
}

// Logs a binder or nvdrv condition once per code (a game may call DequeueBuffer in a loop).
void warn_once(uint32_t code, const char* what) {
    static std::atomic<uint32_t> seen{0};
    const uint32_t bit = 1u << (code & 31);
    if (!(seen.fetch_or(bit) & bit)) NXLOG_W(kTagVi, "binder transaction %u %s", code, what);
}

// Registers a command that is accepted and does nothing (logged at Debug).
void accept_cmd(ipc::Service& s, uint32_t id, const char* name, const char* tag) {
    s.add_cmd(id, name, [name, tag](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D(tag, "%s accepted, nothing done", name);
        return 0;
    });
}

}  // namespace nxrt::services::vinv
