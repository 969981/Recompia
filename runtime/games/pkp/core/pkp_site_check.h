// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the check a module makes of its hook sites once main is loaded, before it arms its hooks: every site holds the
// instruction word the module was written against, and a midhook's site is a hook site of the lifted game libraries (the lifter makes
// it a function of its own; an entry hook's address is a lifted function either way).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nxrt/dispatch.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

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

}  // namespace nxrt::games::pkp
