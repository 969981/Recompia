// SPDX-License-Identifier: GPL-3.0-or-later
// Display tuning that the NVN back end reads on the render thread (S10): texture filtering overrides. Play mode writes them
// from the settings page; the sampler cache includes them in its key, so a change takes effect on the next draw without a restart.
#pragma once
#include <atomic>
#include <cstdint>

namespace nxrt::gfx {

// Shadow map resolution (play settings [display] shadow_quality): the game's shadow maps (square power-of-two depth targets, which the render
// scale leaves alone) are made this many times larger per side. Default: as the game draws them. Live: the maps are remade at the new size.
enum class ShadowQuality : uint8_t { Default = 0, High, VeryHigh, Ultra, Extreme };
inline float shadow_scale_of(ShadowQuality q) {
    return q == ShadowQuality::Extreme ? 8.0f : q == ShadowQuality::Ultra ? 4.0f : q == ShadowQuality::VeryHigh ? 3.0f : q == ShadowQuality::High ? 2.0f : 1.0f;
}

struct Tuning {
    std::atomic<int> anisotropy{0};        // 0 = what the game's samplers ask for; 2, 4, 8, 16 = at least this much on every filtering sampler
    std::atomic<float> lod_bias{0.0f};     // added to the LOD bias of every sampler (negative = sharper)
    std::atomic<float> shadow_scale{1.0f};  // shadow_scale_of(shadow quality): the NVN back end makes the game's shadow maps this many times larger per side
    std::atomic<float> render_scale{0.0f};  // the NVN back end's resolution scale from the game's play settings ([display] render_scale); 0 = not set
    // What the NVN back end presented last (its present path writes them every frame, the F1 page reads them): the game picture handed to the
    // output stage (the window texture's crop in rendered pixels, so the render scale is in it) and the size it is shown at (the swapchain
    // image). 0 = nothing presented yet.
    std::atomic<int> present_src_w{0}, present_src_h{0}, present_out_w{0}, present_out_h{0};
    // A game's live scale on some floats of one fragment shader's uniform block (the game: the fog strength on its height-fog pass). When the
    // scale is not 1, that block is bound from a copy in the upload ring with the chosen floats multiplied; the game's memory is left alone.
    std::atomic<uint64_t> ufs_hash{0};     // the fragment shader's hash (its 16 hex digits as a number); 0 = off
    std::atomic<int> ufs_block{-1};        // the uniform block (NVN index, the N of fp_cN)
    std::atomic<uint32_t> ufs_mask{0};     // bit i: the float at byte offset 4 * i of the block
    std::atomic<float> ufs_scale{1.0f};
    // A game's screen-space reduction compute shader that the game dispatches over the full NVN size of a
    // scene image the back end renders smaller ([gfx] render_scale). The back end dispatches it over the scaled image instead: groups x/y
    // times the image's scale (rounded up), the floats of mask in the uniform block (the size the shader bounds-checks with) times the scale,
    // and texel fetches unscaled. Same result for a shader that bins every pixel of the image and whose consumer uses the bins' shares.
    std::atomic<uint64_t> csr_hash{0};     // the compute shader's hash (its 16 hex digits as a number); 0 = off
    std::atomic<int> csr_block{-1};        // the uniform block (NVN index, the N of cp_cN)
    std::atomic<uint32_t> csr_mask{0};     // bit i: the float at byte offset 4 * i of the block
    // A phone: the GPU time of the last frame whose submissions all finished (the union of their timestamp pairs), in microseconds, and
    // how many such frames there were; the game's dynamic resolution reads it. 0 = no timestamps (or not a phone).
    std::atomic<uint32_t> gpu_frame_us{0};
    std::atomic<uint64_t> gpu_frames{0};
    // A phone's live 3D resolution for the frame rate counter (detail Full): the game layer's percent of the screen this frame, the
    // player's Resolution and whether its dynamic resolution runs; the back end's scene part and output in pixels (w << 16 | h) and the
    // upscaler that enlarged it (0 none, 1 SGSR, 2 bicubic, 3 SGSR 2). 0 = not known yet (or not a phone).
    std::atomic<int> res_pct{0}, res_cap{0}, res_upscaler{0};
    std::atomic<bool> res_dynamic{false};
    std::atomic<int> res_dr_cap{0};        // the thermal cap of the phone's dynamic resolution (percent of the screen), 0 when none
    std::atomic<uint32_t> res_scene_px{0}, res_out_px{0};
};

Tuning& tuning();                          // process-wide

}  // namespace nxrt::gfx
