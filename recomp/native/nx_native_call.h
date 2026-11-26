// SPDX-License-Identifier: GPL-3.0-or-later
// Android build of the lifted code only (nx_native.h): a cache per indirect call site in front of the runtime's call_indirect.
// Most `blr` sites call one target (a virtual call on one class); the runtime's dispatch looks every call up in a per-context
// direct-mapped table (a hash, a 16 KiB table, a call through the PLT). Here a site keeps the slot index of its last target in the
// runtime's function table (nx_fn_index, +1) and calls the slot's function directly while the slot still holds that address. The
// check reads the slot's address on every call (acquire; the runtime writes a slot's function before its address), so a table that
// grew or a replaced function (written in place) never runs stale code; any other target, an HLE thunk or a miss is call_indirect
// as before.
#pragma once
extern "C" const void* nx_fn_slots;
extern "C" uint32_t nx_fn_index(uint64_t guest_addr);
struct NxFnSlot {
    uint64_t addr;
    void (*fn)(Ctx*);
};
// A site word: the slot index + 1 in the low 24 bits (0 = none), misses in the top 8. A site that missed 8 times (several targets, an
// HLE thunk) stops looking and always takes call_indirect, so a polymorphic site does not pay a table lookup per call.
static inline __attribute__((always_inline)) void nx_call_ic(Ctx* c, uint64_t t, uint32_t* site) {
    const uint32_t w = __atomic_load_n(site, __ATOMIC_RELAXED);
    const uint32_t i = w & 0xffffffu;
    const NxFnSlot* s = static_cast<const NxFnSlot*>(__atomic_load_n(&nx_fn_slots, __ATOMIC_ACQUIRE));
    if (i && s) [[likely]] {
        const NxFnSlot* e = s + (i - 1);
        if (__atomic_load_n(&e->addr, __ATOMIC_ACQUIRE) == t) [[likely]]
            return __atomic_load_n(&e->fn, __ATOMIC_RELAXED)(c);
    }
    if (w < 0x08000000u) {
        const uint32_t k = nx_fn_index(t);
        __atomic_store_n(site, (w & 0xff000000u) + 0x01000000u + (k <= 0xffffffu ? k : 0u), __ATOMIC_RELAXED);
    }
    call_indirect(c, t);
}
// Every call_indirect in the lifted code after this point (its declaration in ctx.h was parsed before) gets its own site cache.
#define call_indirect(c, t) nx_call_ic((c), (t), ({ static uint32_t nx_ic_site_; &nx_ic_site_; }))

// The same for imports (the lifted PLT stubs: one host_import call each): a stub keeps the import's leaf handler (strlen and friends)
// and calls it directly while import_epoch's low 16 bits are the ones it was read at; any other import takes host_import as before.
extern "C" uint64_t nx_import_leaf(uint32_t import_id);
extern "C" const uint32_t* const nx_import_epoch_word;
static inline __attribute__((always_inline)) void nx_import_ic(Ctx* c, uint32_t id, uint64_t* site) {
    const uint64_t ep = __atomic_load_n(nx_import_epoch_word, __ATOMIC_ACQUIRE) & 0xffffu;
    uint64_t w = __atomic_load_n(site, __ATOMIC_RELAXED);
    if ((w >> 48) != ep || !w) [[unlikely]] {
        w = nx_import_leaf(id);
        __atomic_store_n(site, w, __ATOMIC_RELAXED);
    }
    if ((w >> 48) == ep && (w & 0xffffffffffffull)) [[likely]]
        return reinterpret_cast<void (*)(Ctx*)>(w & 0xffffffffffffull)(c);
    host_import(c, id);
}
#define host_import(c, id) nx_import_ic((c), (id), ({ static uint64_t nx_imp_site_; &nx_imp_site_; }))
