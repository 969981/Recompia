// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: logs once when the engine's TemporalAA3, TemporalAA (v1) and KTSS passes set up their slots (pkp_taa_probe.cpp). Only built with
// NXRT_TESTTOOLS (the table in pkp_game.cpp). The player-side TAA feature is pkp_taa.h.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kTaaProbeFeature;

}  // namespace nxrt::games::pkp
