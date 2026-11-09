// SPDX-License-Identifier: GPL-3.0-or-later
// Jump momentum (pkp_jump.cpp): a running jump keeps the running speed in the air. [pkp] jump_momentum, off by default.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void jump_install(uint64_t base);

}  // namespace nxrt::games::pkp
