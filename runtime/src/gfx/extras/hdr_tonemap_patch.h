// SPDX-License-Identifier: GPL-3.0-or-later
// HDR patches of a game's tone-map fragment shader (S14). Which of them a game's shader is, and the constants it is matched on, the game
// names (game::Info::render tonemaps, nxrt/game_render.h); a game that names none has no tone-map shader.
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

// HDR patch of a LUT tone-map (KTGL's cbComposite shaders, for one). The shader takes the exposed scene colour x per channel through a log
// shaper, coordinate = k * log2(shaper_scale * x + shaper_bias) + c, into a grading volume whose output is display-referred [0,1]; a LUT that
// clips flat at 1 makes everything brighter paper white. The patch keeps the graded colour and scales it at the end by
//   F = 1 + (R - 1) * (1 - exp(-max(m - 1, 0) / (R - 1))) * smoothstep(0.9, 1, max(rgb out))
// with m the largest channel of the scene colour x and R the peak ratio (peak / paper white): 1 below the clip, slope 1 at the knee and the
// display's peak as the asymptote; the smoothstep keeps colours the grade did not bring near white as they are, so a LUT with a soft shoulder
// is not brightened below it. Hue is the grade's (one factor for all three channels).
// Matched by structure: a fragment shader whose Location 0 vec4 output is written per component, with exactly three GLSL
// Fma(x, shaper_scale, shaper_bias) (the shaper's input per channel; the constants compared bit for bit, named by the game:
// game::ToneMapMatch). R is baked in (the module is made once per shader; a live peak change applies at the next start).
// Returns false (out untouched) when the shader is not one, or peak_ratio <= 1.001.
bool spirv_hdr_tonemap_lut(const std::vector<uint32_t>& in, float shaper_scale, float shaper_bias, float peak_ratio, std::vector<uint32_t>& out);

}  // namespace nxrt::extras
