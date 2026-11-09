// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: a UI widget's text slot set to our own UTF-8 string (a key guide part, pkp_key_guide.cpp; a Settings row,
// pkp_options.cpp), the way the game shows a string argument: text id layout::kStringText ("<string;0(lstr1)>") through
// layout::kTextString (widget, slot, text id, const char*).
#pragma once
#include <cstdint>
#include <cstring>

#include "pkp_guest.h"
#include "pkp_layout.h"

namespace nxrt::games::pkp::text {

// The string is copied below the guest stack pointer (the game reads it as guest memory); the call's stack starts under it.
inline void set_string(::Ctx* c, uint64_t base, uint64_t widget, uint32_t slot, const char* utf8) {
    const size_t n = std::strlen(utf8);
    const uint64_t buf = (c->x[31] - (n + 0x40)) & ~uint64_t{0xf};
    std::memcpy(reinterpret_cast<void*>(buf), utf8, n + 1);
    guest::call(c, base + layout::kTextString, widget, slot, layout::kStringText, buf, 0, buf - 0x10);
}

}  // namespace nxrt::games::pkp::text
