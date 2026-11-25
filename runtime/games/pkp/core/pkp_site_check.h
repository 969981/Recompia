// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the check a module makes of its hook sites once main is loaded, before it arms its hooks: every site holds the
// instruction word the module was written against, and a midhook's site is a hook site of the lifted game libraries (the lifter makes
// it a function of its own; an entry hook's address is a lifted function either way).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nxrt/dispatch.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

constexpr uint32_t kPacibsp = 0xd503237fu;   // pacibsp: the first instruction of most of the game's functions

struct HookSite {
    uint64_t site;   // offset in main
    uint32_t word;   // the instruction expected there
    bool entry;      // an entry hook; else a midhook, run before the instruction
};

// The first of the n sites that is not as expected (*word: what main has there, 0 when unmapped), or null when all are.
inline const HookSite* first_bad_site(uint64_t base, const HookSite* sites, size_t n, uint32_t* word) {
    for (size_t i = 0; i < n; ++i) {
        const uint64_t a = base + sites[i].site;
        *word = 0;
        if (mem::is_mapped(a, 4)) std::memcpy(word, reinterpret_cast<const void*>(a), 4);
        if (*word != sites[i].word || (!sites[i].entry && fn_lookup(a) == nullptr)) return &sites[i];
    }
    return nullptr;
}

// For a site first_bad_site returned (word: what it found there): true when main holds the expected word but the lifted libraries have
// no hook site at it (the game is the right one; pkp.exe was linked before the site was added: Set up again relinks it). False: main
// holds another word (another game version or dump).
inline bool site_missing_from_lift(const HookSite& site, uint32_t word) { return !site.entry && word == site.word; }

// `bl` at main+at calls main+to (word: the instruction at main+at).
constexpr bool is_bl_to(uint32_t word, uint64_t at, uint64_t to) {
    if ((word & 0xfc000000u) != 0x94000000u) return false;
    const int64_t imm = static_cast<int64_t>(static_cast<int32_t>(word << 6) >> 6) * 4;   // imm26, sign-extended, in bytes
    return at + static_cast<uint64_t>(imm) == to;
}
// main+0x7b0a3c: bl 0x376c4.
static_assert(is_bl_to(0x97e21b22u, 0x7b0a3c, 0x376c4) && !is_bl_to(0x97e21b22u, 0x7b0a40, 0x376c4) &&
              !is_bl_to(0x17e21b22u, 0x7b0a3c, 0x376c4), "is_bl_to: a bl to that address only");

}  // namespace nxrt::games::pkp
