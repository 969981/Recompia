// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options (pkp_options.cpp): the PC settings as tabs and rows of the game's own Settings screen. [pkp] options_menu, off by default on testing (on in the Stable release).
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void options_install(uint64_t base);

}  // namespace nxrt::games::pkp
