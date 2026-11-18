// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: the shared state seamless co-op keeps equal on every member, as the test tooling's state lines (nxrt/tt_coop.h, @coop_state,
// [test] coop_state_every), so two runs' logs can be compared for desync. See pkp_coop_probe.cpp. Only built with NXRT_TESTTOOLS (the
// table in pkp_game.cpp). A feature of the registry with a game-frame callback only.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kCoopProbeFeature;

}  // namespace nxrt::games::pkp
