// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim (pkp_aim.cpp): ability and tool targets are aimed where the camera looks, not where the character faces. [pkp] camera_aim,
// off by default in Testing builds.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void aim_install(uint64_t base);

}  // namespace nxrt::games::pkp
