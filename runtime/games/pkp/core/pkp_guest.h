// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: guest memory and guest-call helpers shared by the game hooks (raw reads and writes, the address check, calls into
// lifted game code, the data-table row lookup). Header only; every helper is the exact code the hook files carried before.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nxrt/dispatch.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp::guest {

// Raw guest reads and writes (guest addresses are host addresses). No check: the caller knows the range is mapped.
template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

// The range is above the low 64 GiB (no null-based or small garbage pointer) and mapped.
inline bool ok(uint64_t a, uint64_t n) { return a >= 0x1000000000ull && mem::is_mapped(a, n); }

// Runs the guest function at `fn` (absolute) on `c` with x0..x(N-1) = args (missing ones 0) and, when sp is nonzero, that stack pointer.
// Registers past x(N-1) keep the caller's values. The whole context is restored afterwards; returns x0.
template <size_t N>
uint64_t call_n(::Ctx* c, uint64_t fn, const std::array<uint64_t, N>& args, uint64_t sp = 0) {
    const ::Ctx saved = *c;
    for (size_t i = 0; i < N; ++i) c->x[i] = args[i];
    if (sp) c->x[31] = sp;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}
// The common form: x0..x4.
inline uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t sp = 0) {
    return call_n<5>(c, fn, {a0, a1, a2, a3, a4}, sp);
}

// A row of one of the data-table manager's tables ([0x35618f0] + 0x10 + table offset), as the game looks it up:
// table->vtable[0x38](0)->vtable[0x30](id); 0 if none. N = the argument registers each of the two calls sets (see call_n).
template <size_t N>
uint64_t table_lookup(::Ctx* c, uint64_t table, uint32_t id) {
    const uint64_t rows = table ? call_n<N>(c, rd<uint64_t>(rd<uint64_t>(table) + 0x38), {table, 0}) : 0;
    return rows ? call_n<N>(c, rd<uint64_t>(rd<uint64_t>(rows) + 0x30), {rows, id}) : 0;
}

}  // namespace nxrt::games::pkp::guest
