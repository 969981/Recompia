// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime-provided half of the generated-code ABI (recomp/include/nxrecomp/ctx.h, ABI version 2, section "runtime-provided"):
// call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, guest_mrs, guest_msr, stx_diag. The rest of the section lives next
// to its subsystem: nx_site_inline / nx_site_midhook / nx_hook_entry in hooks.cpp, nx_catch_call in eh/catch_call.cpp, and the
// registration of the lifted module tables (NxModule) in nxmodule.cpp.
// Pattern from smo-recomp runtime/core/hostrt.c (host_import with in_import bookkeeping, stx_diag, guest_trap / guest_unimpl).
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <set>

#include "nxrt/clock.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "nxrt/platform_facts.h"
#include "nxrt/svc.h"
#include "../diag/hle_census.h"

namespace nxrt {
namespace {
std::mutex g_sysreg_lock;
std::set<uint32_t> g_sysregs_seen;
}  // namespace
}  // namespace nxrt

namespace nxrt {
namespace {
// Phones: host_import's per-context cache of import id -> (target, leaf handler). host_import resolved every call through the module
// registry (import_resolve: two dependent loads) and asked whether the target is a leaf thunk (hle_leaf_fn: three more), about a million
// calls a second outdoors (import_resolve + host_import 1.3 % of all samples on a Tensor G2 (Mali-G710)). A context claims a slot as dispatch.cpp's
// cache does (a context runs on one thread at a time); entries are valid for one import_epoch(). A context whose slot another holds
// takes the full path.
struct ImpEnt {
    uint32_t id, epoch;
    uint64_t target;
    HleFn leaf;
};
struct alignas(64) ImpCache {
    std::atomic<const ::Ctx*> owner{nullptr};
    ImpEnt e[1024]{};
};
template <bool kOn = facts::kPhone>
ImpCache* imp_cache(const ::Ctx* c) {
    constinit static ImpCache slots[256];   // zero: in .bss; 24 KiB per slot, touched by the contexts that import
    ImpCache& s = slots[(reinterpret_cast<uintptr_t>(c) * 0x9E3779B97F4A7C15ull) >> 56];
    const ::Ctx* o = s.owner.load(std::memory_order_relaxed);
    if (o == c) return &s;
    if (o == nullptr && s.owner.compare_exchange_strong(o, c, std::memory_order_relaxed)) return &s;
    return nullptr;
}
// Everything but a cached leaf call, out of line, so that the hit path needs no stack frame (a leaf thunk is then a tail call).
template <bool kOn = facts::kPhone>
[[gnu::noinline]] void host_import_slow(::Ctx* c, uint32_t import_id, ImpEnt* e, uint32_t ep) {
    uint64_t target;
    HleFn leaf;
    if (e && e->id == import_id && e->epoch == ep && e->target) {
        target = e->target;
        leaf = e->leaf;
    } else {
        target = import_resolve(import_id);
        if (!target) {
            char what[96];
            snprintf(what, sizeof what, "host_import with an unknown import id (module %u, symbol %u)", NX_IMPORT_MODULE(import_id), NX_IMPORT_SYM(import_id));
            trap(what, import_id);
        }
        leaf = hle_leaf_fn(target);
        if (e) *e = ImpEnt{import_id, ep, target, leaf};
    }
    if (leaf) {   // strlen and friends: no bookkeeping
        if (diag::census::on()) [[unlikely]] {
            diag::census::Scope scope(diag::census::key(diag::census::kHle, static_cast<uint32_t>((target - mem::kHleBase) / 16)));
            return leaf(c);
        }
        return leaf(c);
    }
    detail::count_import();
    const uint32_t saved_id = c->import_id;
    const uint64_t saved_from = c->import_from;
    c->import_id = import_id;
    c->import_from = c->x[30] - 4;
    dispatch_call(c, target);
    c->import_id = saved_id;
    c->import_from = saved_from;
}
template <bool kOn = facts::kPhone>
[[gnu::always_inline]] inline void host_import_phone(::Ctx* c, uint32_t import_id) {
    const uint32_t ep = import_epoch().load(std::memory_order_acquire);
    ImpCache* ic = imp_cache(c);
    ImpEnt* e = ic ? &ic->e[(import_id * 0x9E3779B1u) >> 22] : nullptr;
    if (e && e->id == import_id && e->epoch == ep && e->leaf && !diag::census::on()) [[likely]] return e->leaf(c);
    return host_import_slow(c, import_id, e, ep);
}
}  // namespace
}  // namespace nxrt

extern "C" {

void call_indirect(Ctx* c, uint64_t addr) { nxrt::dispatch_call(c, addr); }

// A call to a symbol another module provides: the lifted PLT stub passes NX_IMPORT_ID(module index, symbol index) of its own
// module. The registry (nxmodule.cpp) names the import and finds its target: a function another lifted module defines, else the HLE
// handler or logging stub of that name.
void host_import(Ctx* c, uint32_t import_id) {
    if constexpr (nxrt::facts::kPhone) return nxrt::host_import_phone(c, import_id);
    const uint64_t target = nxrt::import_resolve(import_id);
    if (!target) {
        char what[96];
        snprintf(what, sizeof what, "host_import with an unknown import id (module %u, symbol %u)", NX_IMPORT_MODULE(import_id), NX_IMPORT_SYM(import_id));
        nxrt::trap(what, import_id);
    }
    // Phones count the non-leaf imports only (below): the count is a statistic, and each count is a thread_local access, which in the
    // dlopened Android library is a call of the TLSDESC resolver (leaf calls: about a million a second outdoors).
    if constexpr (!nxrt::facts::kPhone) nxrt::detail::count_import();
    if (nxrt::HleFn leaf = nxrt::hle_leaf_fn(target)) {   // strlen and friends: tens of millions of calls in a boot, no bookkeeping
        if (nxrt::diag::census::on()) [[unlikely]] {   // [diag] hle_census: a leaf thunk is counted here (it never reaches dispatch_call)
            nxrt::diag::census::Scope scope(nxrt::diag::census::key(nxrt::diag::census::kHle, static_cast<uint32_t>((target - nxrt::mem::kHleBase) / 16)));
            return leaf(c);
        }
        leaf(c);
        return;
    }
    // Only the id is stored: resolving the name here cost a module lookup on every non-leaf import call (thousands a frame), and
    // only a freeze or crash report ever reads it (diag/watchdog.cpp, which resolves it then).
    if constexpr (nxrt::facts::kPhone) nxrt::detail::count_import();
    const uint32_t saved_id = c->import_id;
    const uint64_t saved_from = c->import_from;
    c->import_id = import_id;
    c->import_from = c->x[30] - 4;
    nxrt::dispatch_call(c, target);
    c->import_id = saved_id;
    c->import_from = saved_from;
}

void guest_trap(const char* what, uint64_t addr) { nxrt::trap(what, addr); }

void guest_unimpl(uint64_t pc, uint32_t insn, const char* what) {
    NXLOG_E("guest", "UNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)", static_cast<unsigned long long>(pc), insn, what);
    nxrt::log_flush();
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void guest_svc(Ctx* c, uint32_t number) { nxrt::svc_dispatch(c, number, 0); }

// System registers the lifter does not model inline: the generic timer counters are real (the system tick, nxrt/clock.h), other reads give 0, writes are ignored; each register is reported once.
uint64_t guest_mrs(Ctx* c, uint32_t sysreg) {
    switch (sysreg) {                       // the generic timer: the SDK reads CNTPCT_EL0 33 times (nn::os::GetSystemTick and friends)
        case 0xdf00: return nxrt::clock::tick_hz();                  // CNTFRQ_EL0: 19.2 MHz (Switch 1) or 31.25 MHz (Switch 2)
        case 0xdf01:                                                // CNTPCT_EL0
        case 0xdf02: return nxrt::clock::ticks();                   // CNTVCT_EL0
        default: break;
    }
    std::lock_guard<std::mutex> lk(nxrt::g_sysreg_lock);
    if (nxrt::g_sysregs_seen.insert(sysreg).second) NXLOG_W("guest", "MRS of system register 0x%04x (not modelled): returns 0 (in f_%llx)", sysreg, static_cast<unsigned long long>(c->fn));
    return 0;
}

void guest_msr(Ctx* c, uint32_t sysreg, uint64_t value) {
    std::lock_guard<std::mutex> lk(nxrt::g_sysreg_lock);
    if (nxrt::g_sysregs_seen.insert(sysreg | 0x10000u).second) NXLOG_W("guest", "MSR to system register 0x%04x = 0x%llx (not modelled): ignored (in f_%llx)", sysreg, static_cast<unsigned long long>(value), static_cast<unsigned long long>(c->fn));
}

// Called on every failed exclusive store; reports a loop that never succeeds.
void stx_diag(Ctx* c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    uint64_t n = ++c->ex_fail;
    if (n == 1000 || n == 100000)
        NXLOG_W("guest", "STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(a), static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(seen), static_cast<unsigned long long>(v), bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
}

}  // extern "C"
