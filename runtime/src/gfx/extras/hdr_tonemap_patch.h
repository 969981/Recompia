// SPDX-License-Identifier: GPL-3.0-or-later
// HDR patch of the game's tone-map fragment shader (S14).
//
// The shader maps the scene colour through a per-channel curve (cubic toe, linear middle, rational shoulder) into [0,1], samples two 3D
// colour-grading volumes with the curve output as coordinate and writes the result. With the shoulder moved to a display's peak by the runtime
// (extras::extend_shoulder on the 80-byte parameter block), the curve output y can exceed 1; the volumes clamp there, so the highlights would be
// lost. The patch multiplies each graded channel by max(y, 1): the grade is kept and the part of the curve above 1 is added on top, per channel.
// With y <= 1 everywhere (the unextended curve) the patched shader writes exactly what the original writes.
//
// The match is by structure, not by hash: the module must have a fragment entry point with exactly two 3D sampled images whose first sample's
// coordinate is a CompositeConstruct of three GLSL Fma results (curve, scale, bias). Anything else is left alone (the function returns false).
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::extras {

bool spirv_hdr_tonemap(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

}  // namespace nxrt::extras
