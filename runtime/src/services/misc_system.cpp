// SPDX-License-Identifier: GPL-3.0-or-later
// csrng, ldr:ro and mm:u (see misc.cpp for the shared provenance and rules).
#include "misc_internal.h"

#include "nxrt/entropy.h"

namespace nxrt::services::miscsvc {

// ---- csrng ----------------------------------------------------------------------------------------------------------------------
// GenerateRandomBytes(buffer<u8, 6> data): fills the output buffer from the Windows CSPRNG (the seeded stream in deterministic test
// mode, so replays see the same random numbers).
std::shared_ptr<Service> make_csrng() {
    return make_iface("csrng", [](Service& s) {
        s.add_cmd(0, "GenerateRandomBytes", [](Request& req, Response&) -> uint32_t {
            const ipc::Buffer b = req.out_buf(0);
            if (!entropy::fill(buf_mut(b), static_cast<size_t>(b.size))) return kNotSupported;   // the OS generator failed (logged)
            NXLOG_D("csrng", "GenerateRandomBytes: %llu bytes", static_cast<unsigned long long>(b.size));
            return 0;
        });
    });
}

// ---- ldr:ro -----------------------------------------------------------------------------------------------------------------------
// The title loads no NRO. RegisterProcessHandle (cmd 4) is what the SDK sends first (the "Initialize" of nn::ro); the module-loading
// commands answer NotLoaded.
std::shared_ptr<Service> make_ro() {
    return make_iface("IRoInterface", [](Service& s) {
        s.add_cmd(0, "MapManualLoadModuleMemory", refuse(kRoNotLoaded, "ro", "MapManualLoadModuleMemory"));
        s.add_cmd(1, "UnmapManualLoadModuleMemory", refuse(kRoNotLoaded, "ro", "UnmapManualLoadModuleMemory"));
        s.add_cmd(2, "RegisterModuleInfo", refuse(kRoNotLoaded, "ro", "RegisterModuleInfo"));
        s.add_cmd(3, "UnregisterModuleInfo", refuse(kRoNotLoaded, "ro", "UnregisterModuleInfo"));
        s.add_cmd(4, "RegisterProcessHandle", accept("ro", "RegisterProcessHandle"));
        s.add_cmd(10, "RegisterModuleInfoWithUserProcessHandle", refuse(kRoNotLoaded, "ro", "RegisterModuleInfoWithUserProcessHandle"));
    });
}

MmState& mm_state() {
    static MmState s;
    return s;
}

namespace {

MmSlot* mm_find(MmState& s, bool by_module, uint32_t key) {
    for (auto& sl : s.slots)
        if (sl.used && sl.by_module == by_module && sl.key == key) return &sl;
    return nullptr;
}
// Keeps a request. The client gets its id even when no slot is free (it then reads back 0), as ScarletNX does.
uint32_t mm_open(uint32_t module, bool by_module, uint32_t module_for_id) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    const uint32_t id = ++s.next_id;
    const uint32_t key = by_module ? module : id;
    bool kept = false;
    for (auto& sl : s.slots) {
        if (sl.used) continue;
        sl.used = true;
        sl.by_module = by_module;
        sl.key = key;
        sl.module = by_module ? module : module_for_id;
        sl.current = 0;
        kept = true;
        break;
    }
    if (!kept) NXLOG_W("mm", "more than %zu clock requests, request %u is not kept", kMmSlots, id);
    return by_module ? 0 : id;
}
void mm_close(bool by_module, uint32_t key) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    if (MmSlot* sl = mm_find(s, by_module, key)) sl->used = false;
}
void mm_set(bool by_module, uint32_t key, uint32_t value) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    if (MmSlot* sl = mm_find(s, by_module, key)) sl->current = value;
}
uint32_t mm_get(bool by_module, uint32_t key) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    const MmSlot* sl = mm_find(s, by_module, key);
    return sl ? sl->current : 0;
}

}  // namespace

std::shared_ptr<Service> make_mm() {
    return make_iface("mmnv::IRequest", [](Service& s) {
        // Old forms: module at +0, then priority / min / max at +4 / +8.
        s.add_cmd(0, "InitializeOld", [](Request& req, Response&) -> uint32_t {
            mm_open(req.arg<uint32_t>(0), true, 0);
            NXLOG_D("mm", "InitializeOld module %u", req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(1, "FinalizeOld", [](Request& req, Response&) -> uint32_t {
            mm_close(true, req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(2, "SetAndWaitOld", [](Request& req, Response&) -> uint32_t {
            mm_set(true, req.arg<uint32_t>(0), req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(3, "GetOld", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(mm_get(true, req.arg<uint32_t>(0)));
            return 0;
        });
        // New forms: request id returned by Initialize, used by the rest.
        s.add_cmd(4, "Initialize", [](Request& req, Response& rsp) -> uint32_t {
            const uint32_t id = mm_open(req.arg<uint32_t>(0), false, req.arg<uint32_t>(0));
            NXLOG_D("mm", "Initialize module %u -> request %u", req.arg<uint32_t>(0), id);
            rsp.push<uint32_t>(id);
            return 0;
        });
        s.add_cmd(5, "Finalize", [](Request& req, Response&) -> uint32_t {
            mm_close(false, req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(6, "SetAndWait", [](Request& req, Response&) -> uint32_t {
            mm_set(false, req.arg<uint32_t>(0), req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(7, "Get", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(mm_get(false, req.arg<uint32_t>(0)));
            return 0;
        });
    });
}

}  // namespace nxrt::services::miscsvc
