// SPDX-License-Identifier: GPL-3.0-or-later
// What the graphics back end may know about one game's rendering (game::Info::render, docs/GAME_BOUNDARY.md). Pure data: the NVN back end
// and the extras read it once, the first time they need it, so it costs the per-draw paths a compare against a cached value at most.
// Every part is optional. A null part turns off what needs it:
//   readback_defer   none: [nvn] readback_defer never defers a read
//   camera           none: no temporal upscaling (FSR 3 / DLSS / DLAA find no camera, nothing is jittered)
//   tonemaps         none: no shader is the tone map (no HDR variant, no scene upscaler, no temporal pass)
//   loop_guards      none: no compute loop gets its count limited ([nvn] loop_guard has nothing to do)
// Header-only and free of Vulkan, so game.h users that do not link the graphics back end can include it.
#pragma once
#include <cstdint>

namespace nxrt::game {

// [nvn] readback_defer (#276): the one texture shape whose CPU reads (nvnTextureReadTexels) may be served from the previous read while this
// read's copy is queued, so the reading thread never waits for the GPU. Name a small render target the game reads again and again and
// whose consumer tolerates data one read period old (on the console these reads are plain memory reads that get whatever the GPU had
// finished). Every image of this format, extent, level and layer count qualifies; a texture id would not, ids change between sessions.
struct ReadbackShape {
    uint32_t vk_format = 0;                 // the host image's VkFormat (after the back end's format choice)
    uint32_t width = 0, height = 0, depth = 0;
    uint32_t levels = 0, layers = 0;
};

// The scene camera for temporal upscaling (src/gfx/extras/temporal.h): a uniform block of the G-buffer pass's shaders that holds the
// view-projection and the view matrix as floats (column-major 4x4 each). The back end jitters the view-projection of every bound copy of the
// block and reads the camera from the first G-buffer draw of a frame. The block must be at least (max(view_proj, view) + 16) floats long.
struct CameraBlock {
    uint8_t stage = 0;                      // the NVN shader stage the block is bound to (0 vertex, 1 fragment, 2 geometry, 3 tess control,
                                            // 4 tess eval)
    uint8_t slot = 0;                       // its NVN uniform buffer slot in that stage
    uint8_t gbuffer_colors = 0;             // a draw with at least this many colour targets and a depth target is a G-buffer draw
    uint32_t view_proj = 0;                 // float index of the view-projection (jittered by the game when it has its own TAA)
    uint32_t view = 0;                      // float index of the view matrix (an orthonormal rotation plus translation)
    // History reset on a camera cut: the camera moved more than max(cut_min, cut_per_ms * frame ms) world units since the last frame.
    double cut_min = 0, cut_per_ms = 0;
};

// The game's tone-map fragment shader, matched by structure (src/gfx/extras/hdr_tonemap_patch.h). The first entry that matches a shader wins.
enum class ToneMapKind : uint8_t {
    Curve,                                  // a per-channel curve into two 3D grading volumes (spirv_hdr_tonemap)
    LutShaper,                              // a log shaper Fma(x, shaper_scale, shaper_bias) per channel into a grading LUT (spirv_hdr_tonemap_lut)
};
struct ToneMapMatch {
    ToneMapKind kind = ToneMapKind::Curve;
    uint32_t max_words = 0;                 // shaders with more SPIR-V words are not tried with this entry (0: no limit)
    float shaper_scale = 0, shaper_bias = 0;   // LutShaper: the shaper's Fma constants, compared bit for bit
};

// [nvn] loop_guard: a compute shader whose loop count is a constant buffer word and that walks a texel buffer table (spirv_limit_count in
// src/gfx/nvn/nvn_spirv_patch.h). Names are the shader translator's variable names; one entry per count (a shader may have several).
struct LoopGuard {
    const char* shader = nullptr;           // the shader's cache hash, as the back end names it ("0f9696499923d6d3")
    const char* count_var = nullptr;        // the uniform block that holds the count ("cp_c3")
    uint32_t word = 0;                      // the count's 32-bit word in that block
    const char* table_var = nullptr;        // the texel buffer the loop walks ("cp_t_cb0_10")
    uint32_t stride = 1;                    // texels an item
};

struct Render {
    const ReadbackShape* readback_defer = nullptr;
    const CameraBlock* camera = nullptr;
    const ToneMapMatch* tonemaps = nullptr;
    uint32_t tonemap_count = 0;
    const LoopGuard* loop_guards = nullptr;
    uint32_t loop_guard_count = 0;
};

}  // namespace nxrt::game
