// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Pokemon actor census used by the resident cap feature.
#pragma once
#include <cstdint>
namespace nxrt::games::pkp::resident_census {
void start(std::uint64_t main_base);
void tick();
void stop();
}
