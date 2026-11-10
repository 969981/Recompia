// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, PC picture quality above the console's (pkp_quality.cpp): a quality preset that sets the renderer's texture sharpness
// ([display] lod_bias, anisotropy). The view distance is fixed at the console's (#249). Wired into play mode by pkp_game.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/pkp_feature.h"
#include "play_types.h"

namespace nxrt::games::pkp {

extern const Feature kQualityFeature;
// F1 Graphics page: the values of a preset written into d. "switch2", "high", "ultra": texture LOD bias, anisotropy;
// "steamdeck", "legion_go_s", "laptop", "desktop": those and the render scale and frame limit. False for an unknown name (and "custom").
bool quality_preset(const char* name, play::Settings& s);

}  // namespace nxrt::games::pkp
