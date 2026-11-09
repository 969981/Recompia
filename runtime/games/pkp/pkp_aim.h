// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim (pkp_aim.cpp): ability and tool targets are aimed where the camera looks, not where the character faces. [pkp] camera_aim,
// off by default.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void aim_install(uint64_t base);
// Once per frame (main thread): pins the Joy-Con 2 mouse pointer for the game's mouse mode while camera aim applies.
void aim_frame(bool in_game);

}  // namespace nxrt::games::pkp
