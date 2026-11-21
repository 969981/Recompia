// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the game's text language, read once here for every module (our own translated strings, pkp_strings.h; the crafting
// search's words, the title's and the pause menu's key guide labels; the name slot of the save).
//
// The game, 2.0.1: [main + layout::kLanguage] -> u32, the index of the language its text banks are in; null before the game set it.
// The order is the game's own (the order of its text banks; the name keyboard takes 12 characters for 1..5 and 6 for the others, P0-RE):
//   0 Japanese, 1 English, 2 French, 3 Italian, 4 German, 5 Spanish, 6 Traditional Chinese, 7 Korean, 8 Simplified Chinese.
// Read at every call: the player can change it in the game's Settings. Unset, or out of range (a language pack may add a slot of its
// own), reads as English.
#pragma once
#include <cstdint>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::language {

enum class Lang : uint32_t {
    Japanese = 0,
    English = 1,
    French = 2,
    Italian = 3,
    German = 4,
    Spanish = 5,
    ChineseTraditional = 6,
    Korean = 7,
    ChineseSimplified = 8,
};
inline constexpr uint32_t kCount = 9;

inline Lang current(uint64_t base) {
    const uint64_t p = guest::rd<uint64_t>(base + layout::kLanguage);
    const uint32_t l = p && guest::ok(p, 4) ? guest::rd<uint32_t>(p) : static_cast<uint32_t>(Lang::English);
    return l < kCount ? static_cast<Lang>(l) : Lang::English;
}

inline uint32_t index(uint64_t base) { return static_cast<uint32_t>(current(base)); }

}  // namespace nxrt::games::pkp::language
