// SPDX-License-Identifier: GPL-3.0-or-later
// Upscaler hooks of the replay: temporal upscaling (camera block, jitter, the tone-map draw), SMAA at the tone map and the scene upscaler at the
// game's scene stretch draw.
#include "nvn_replay_internal.h"
#include "extras/fsr1_spatial.h"
#include "extras/smaa_scene.h"
#include "extras/sgsr_spatial.h"

#include "nxrt/game_render.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/platform_facts.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
SceneOverride g_su;
TsrBlock g_tsr;
static Ref<Texture> g_tsr_depth;                   // temporal upscaling: the depth target of the G-buffer draw that gave this frame's camera
static uint64_t g_tsr_frame = 0;                    // temporal upscaling: tone-map draws so far (the jitter phase of the frame being drawn)
// Temporal upscaling at the frame's scene enlargement (temporal_deferred). g_tsr_stretch_frame: the frame (g_tsr_frame) whose scene
// stretch last enlarged the scene; g_tsr_deferred: this frame's tone map left the temporal pass to that draw.
static uint64_t g_tsr_stretch_frame = 0;
static bool g_tsr_deferred = false;
void tsr_resolve() {
    g_tsr.resolved = true;
    const game::CameraBlock* cb = extras::temporal_camera_block();
    if (!cb) return;
    g_tsr.stage = cb->stage;
    g_tsr.slot = cb->slot;
    g_tsr.bytes = extras::temporal_camera_bytes();
    g_tsr.colors = cb->gbuffer_colors;
}
// Temporal upscaling: a copy of the game's camera block with this frame's jitter added (extras::temporal_jitter_block). False: left alone.
bool tsr_jitter(uint32_t* words, size_t n_words) {
    Texture* d = S.depth_tex.get();
    if (!d || !tex_gpu(d)) return false;
    const VkExtent3D e = tex_image_extent(d, 0);
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);   // the scene's viewport is that part of the target: half a pixel of it
    const uint32_t w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(e.width * fx)));
    const uint32_t h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(e.height * fy)));
    return extras::temporal_jitter_block(words, n_words, w, h, g_tsr_frame);
}

// The temporal pass ran in the frame before (temporal_run gave a result): only then is the camera jittered (tsr_cam_copy), so a frame
// that is shown without the pass (no camera found, the stretch never came, the mode just switched on) is not a raw jittered frame (only the first frame without the pass can be, the one that shows it lost).
static bool g_tsr_live = false;
bool tsr_cam_copy(uint64_t addr, uint32_t* words) {
    if (!g_tsr_live || g_tsr.bytes > kTsrMaxWords * 4 || !gw_settled_fast(addr, g_tsr.bytes) || out_overlaps(addr, g_tsr.bytes)) return false;
    std::memcpy(words, host_ptr(addr), g_tsr.bytes);
    return tsr_jitter(words, g_tsr.bytes / 4);
}

// Copies of the scene camera's view-projection sit in other blocks too (G-buffer passes with their own block layout, the decal
// and lighting passes) and its inverse in the lighting passes (world positions from the depth). Jittering only the camera block made the
// frame inconsistent: some geometry was drawn jittered and some not, and the lighting rebuilt positions from the jittered depth with the
// unjittered inverse, so some faces fell into shade depending on the offset (dark walls, looks switching between frames). Every block bound
// while the frame has a jittered camera is searched once (per stage, slot, fragment program and size) for the frame's unjittered
// view-projection or its inverse; where found, the binding is copied with the jittered matrix (or its inverse) in its place.
namespace {
struct OtherCam { std::vector<std::pair<uint32_t, uint8_t>> at; };   // float offset, 1 view-projection / 2 its inverse
std::unordered_map<uint64_t, OtherCam> g_other;
const OtherCam* g_other_hit = nullptr;   // the bind being prepared (tsr_other_wanted -> tsr_other_patch)
float g_m[4][16];                        // this frame's view-projection, its inverse, jittered, jittered inverse
bool near16(const float* a, const float* b) {
    float mx = 0;
    for (int i = 0; i < 16; ++i) mx = std::max(mx, std::fabs(b[i]));
    for (int i = 0; i < 16; ++i) if (!(std::fabs(a[i] - b[i]) <= 1e-3f * mx)) return false;
    return mx > 0;
}
}  // namespace

bool tsr_other_wanted(int stage, int slot, uint64_t addr, uint64_t size) {
    g_other_hit = nullptr;
    if (!g_tsr_live || size < 64 || !extras::temporal_frame_matrices(g_m[0], g_m[1], g_m[2], g_m[3])) return false;
    const uint64_t prog = reinterpret_cast<uintptr_t>(S.stage[kFragment]);
    const uint64_t key = (prog * 0x9E3779B97F4A7C15ull) ^ (size << 8) ^ (static_cast<uint64_t>(stage) << 4) ^ static_cast<uint64_t>(slot);
    const size_t nf = static_cast<size_t>(std::min<uint64_t>(size, 65536) / 4);
    if (!gw_settled_fast(addr, nf * 4) || out_overlaps(addr, nf * 4)) return false;
    auto it = g_other.find(key);
    if (it == g_other.end()) {   // first time: where this block holds the camera
        OtherCam oc;
        const float* f = reinterpret_cast<const float*>(host_ptr(addr));
        for (size_t o = 0; o + 16 <= nf; o += 4) {
            if (near16(f + o, g_m[0])) oc.at.push_back({static_cast<uint32_t>(o), 1});
            else if (near16(f + o, g_m[1])) oc.at.push_back({static_cast<uint32_t>(o), 2});
        }
        static int told = 0;
        if (!oc.at.empty() && told < 40) {
            ++told;
            NXLOG_I(kTag, "temporal: the camera's %s also at stage %d slot %d float %u (fs %s, %llu-byte block): jittered there too", oc.at[0].second == 1 ? "view-projection"
                    : "inverse view-projection", stage, slot, oc.at[0].first, S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-", static_cast<unsigned long long>(size));
        }
        it = g_other.emplace(key, std::move(oc)).first;
    }
    if (it->second.at.empty()) return false;
    g_other_hit = &it->second;
    return true;
}

void tsr_other_patch(uint8_t* words, size_t bytes) {
    if (!g_other_hit) return;
    for (const auto& a : g_other_hit->at) {
        if ((a.first + 16) * 4 > bytes) continue;
        float* f = reinterpret_cast<float*>(words) + a.first;
        if (a.second == 1 && near16(f, g_m[0])) std::memcpy(f, g_m[2], sizeof g_m[2]);   // still this frame's matrix: the jittered one in its place
        else if (a.second == 2 && near16(f, g_m[1])) std::memcpy(f, g_m[3], sizeof g_m[3]);
    }
    g_other_hit = nullptr;
}

// Scene upscaler (S14, #571, src/gfx/extras/hdr_nvn.h): before the game's scene stretch draw, FSR 1 / NIS enlarge the scene part of its
// input to the target's size and the draw samples that instead, or Nearest gives the input the nearest sampler (scene_stretch_for_draw).
// The override lives until the next draw.
// Temporal upscaling (FSR 3 / DLSS / DLAA, src/gfx/extras/temporal.h): a G-buffer draw (the game's count of colour targets and a depth target)
// offers its binding of the game's camera block; the first valid one of the frame is kept with its depth target. Called only while
// extras::temporal_wanted(), so the game names a block and g_tsr holds it.
void temporal_gbuffer_note() {
    if (S.ncolor < g_tsr.colors || !S.depth_tex || !S.stage[g_tsr.stage] || extras::temporal_camera_ready()) return;
    const GState::Bind& u = S.ubo[g_tsr.stage][g_tsr.slot];
    if (!u.addr || u.size < g_tsr.bytes) return;
    // guest memory holds the block only when no GPU write is in flight over it (the draw then binds the GPU's copy, unjittered)
    if (!gw_settled_fast(u.addr, g_tsr.bytes) || out_overlaps(u.addr, g_tsr.bytes)) return;
    uint32_t w[kTsrMaxWords];
    const size_t n = static_cast<size_t>(g_tsr.bytes / 4);
    std::memcpy(w, host_ptr(u.addr), n * 4);
    if (g_tsr_live) tsr_jitter(w, n);                                     // the camera as the draw sees it (tsr_cam_copy jitters the bound copy alike)
    extras::temporal_note_gbuffer(w, n);
    if (extras::temporal_camera_ready()) g_tsr_depth = S.depth_tex;
}

// The game's scene covers only part of its targets (extras::scene_fraction, the game's [upscale] scene_scale, #116): the part of a scene
// image of size `scene` that holds it, and the size of the image the draw that reads it then needs, out / fraction, so that its own lookup
// at uv * fraction lands on the upscaled result in that image's top-left out-sized corner. False: the whole image is the scene.
static bool scene_part(VkExtent2D scene, VkExtent2D out, VkExtent2D* part, VkExtent2D* out_tex, const Texture* scene_tex = nullptr) {
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);
    if constexpr (facts::kPhone) {
        // the part this frame drew (the published fraction can belong to the next frame: under dynamic resolution one frame per step
        // enlarged the wrong part)
        uint32_t pw = 0, ph = 0;
        if (scene_tex && frame_scene_part(scene_tex, scene.width, scene.height, &pw, &ph)) {
            fx = pw >= scene.width ? 1.0f : static_cast<float>(pw) / static_cast<float>(scene.width);
            fy = ph >= scene.height ? 1.0f : static_cast<float>(ph) / static_cast<float>(scene.height);
        }
    }
    if (fx >= 1.0f && fy >= 1.0f) return false;
    part->width = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(scene.width * fx)), 1, scene.width);
    part->height = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(scene.height * fy)), 1, scene.height);
    // rounded down: fraction * out_tex <= out, so the draw's lookups at uv * fraction stay inside the written out-sized corner
    out_tex->width = std::max<uint32_t>(out.width, static_cast<uint32_t>(out.width / fx));
    out_tex->height = std::max<uint32_t>(out.height, static_cast<uint32_t>(out.height / fy));
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "scene upscale: the scene covers %ux%u of its %ux%u target (fraction %.3f x %.3f): %ux%u -> %ux%u, handed to the draw in a "
                "%ux%u image", part->width, part->height, scene.width, scene.height, fx, fy, part->width, part->height, out.width, out.height,
                out_tex->width, out_tex->height);
    }
    return true;
}

// The temporal pass (extras::temporal_evaluate) on a scene image: `src` (a view of `tex` in its sampled layout) holds the scene in its
// top-left `render` texels; the result is `out` large (in an out_tex image when that is larger). hdr: the image is the scene before the
// tone map (the tone-map draw), else display-referred colour (a scene enlargement after it). Null: nothing recorded.
static VkImageView temporal_run(Texture* tex, VkImageView src, VkExtent2D render, VkExtent2D out, VkExtent2D out_tex, bool hdr) {
    g_tsr_deferred = false;
    g_tsr_live = false;
    Texture* depth = g_tsr_depth.get();
    static bool told_depth = false;
    if (!extras::temporal_camera_ready()) {                              // counts the frames without a camera (warns after 120)
        extras::temporal_evaluate(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, {}, {}, false);
        return VK_NULL_HANDLE;
    }
    if (!tex || !src || !depth || !tex_gpu(depth)) {
        if (!told_depth) {
            told_depth = true;
            NXLOG_W(kTag, "temporal: camera found but no usable scene image / G-buffer depth texture");
        }
        return VK_NULL_HANDLE;
    }
    const VkExtent3D de = tex_image_extent(depth, 0);
    render.width = std::min(render.width, de.width);
    render.height = std::min(render.height, de.height);
    tex_transition(depth, *F, tex_sampled_layout(depth));
    ViewRequest dr;
    dr.depth_aspect = true;
    VkImageView dv = tex_sampled_view(depth, dr);
    if (!dv) return VK_NULL_HANDLE;
    VkImageView v = extras::temporal_evaluate(&device(), F->cb, src, dv, tex_sampled_layout(depth), render, out, S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W,
                                              out_tex, hdr);
    g_tsr_live = v != VK_NULL_HANDLE;
    g_cmd.dirty = kAllDirty;                                                  // the passes bind their own compute state
    shadow_invalidate();
    return v;
}

// Where the frame's temporal pass runs: at the draw that enlarges the scene, so the result is upscaled once, straight to the size it is
// shown at (#232: at the tone map, a scene part was upscaled, then drawn back into the part by the tone map and stretched bilinearly by
// the game's own stretch: every temporal mode looked like Off). That draw is the game's scene stretch (scene_stretch_for_draw: the copy of
// a scene part over its targets, or the composite into the window texture under a render scale with [gfx] ui_native; the replay treats
// both as the frame's first enlarging stretch). True: a scene part, or the stretch enlarged the scene in the frame before; false: nothing
// enlarges the scene, the tone map runs it (native anti-aliasing on the HDR scene).
static bool temporal_deferred() {
    if (g_tsr_stretch_frame && g_tsr_stretch_frame + 1 >= g_tsr_frame) return true;
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);
    return fx < 1.0f || fy < 1.0f;
}

// The tone-map draw with a temporal mode: the largest 2D colour input is the HDR scene; the result (target size) replaces it for this draw.
static void temporal_for_draw(const ProgLayout& pl) {
    Texture* target = S.color_tex[0].get();
    static bool told_intercept = false, told_input = false;
    if (!told_intercept) {
        told_intercept = true;
        NXLOG_I(kTag, "temporal: tone-map draw %s intercepted (target %ux%u)", S.stage[kFragment]->hash.c_str(), target ? target->b.width : 0,
                target ? target->b.height : 0);
    }
    if (!tex_gpu(target)) return;
    Texture* depth = g_tsr_depth.get();
    const BindingDesc* best = nullptr;
    TexResolved best_r{};
    uint64_t best_area = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex) || (depth && shares_image(r.tex, depth))) continue;
        const VkExtent3D e = tex_image_extent(r.tex, 0);
        const uint64_t area = static_cast<uint64_t>(e.width) * e.height;
        if (area > best_area) { best_area = area; best = &d; best_r = r; }
    }
    if (!best) {
        if (!told_input) {
            told_input = true;
            NXLOG_W(kTag, "temporal: the tone-map draw samples no 2D colour texture to use as the scene");
        }
        temporal_run(nullptr, VK_NULL_HANDLE, {}, {}, {}, true);
        return;
    }
    const DescriptorBinding& b = best->shader->refl.bindings[best->index];
    const VkExtent3D ce = tex_image_extent(best_r.tex, 0), te = tex_image_extent(target, 0);
    VkExtent2D render{ce.width, ce.height}, out_tex{};
    if (VkExtent2D part{}; scene_part(render, VkExtent2D{te.width, te.height}, &part, &out_tex)) render = part;
    end_pass();                                                           // compute work cannot run inside a render pass
    tex_ensure_current(best_r.tex, *F);
    tex_transition(best_r.tex, *F, tex_sampled_layout(best_r.tex));
    VkImageView src = tex_sampled_view(best_r.tex, make_request(b, best_r));
    VkImageView out = temporal_run(best_r.tex, src, render, VkExtent2D{te.width, te.height}, out_tex, true);
    if (out) {
        g_su.tex = best_r.tex;
        g_su.view = out;
    }
}

// SMAA ([upscale] aa): the HDR scene the tone-map draw reads goes through extras::SmaaScene and the draw reads the result, so only the 3D picture is
// filtered (the HUD and the menus are drawn after the tone map). The scene is the first 2D colour input that is a float image (RGBA16F or R11G11B10F) of the target's size.
static void smaa_scene_for_draw(const ProgLayout& pl, VkExtent3D te, extras::AaMode aa) {
    struct Seen { uint32_t w, h; VkFormat f; } tried[8];
    uint32_t tried_n = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D || b.image.depth) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex) || shares_image(r.tex, S.depth_tex.get())) continue;
        const VkExtent3D se = tex_image_extent(r.tex, 0);
        const VkFormat sf = tex_vk_format(r.tex);
        if (static_cast<int>(tried_n) < 8) tried[tried_n++] = {se.width, se.height, sf};
        if (se.width != te.width || se.height != te.height || (sf != VK_FORMAT_R16G16B16A16_SFLOAT && sf != VK_FORMAT_B10G11R11_UFLOAT_PACK32)) continue;
        extras::SmaaScene* sm = extras::scene_smaa(&device());
        if (!sm) return;
        end_pass();                                                           // the passes cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
        VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
        if (!src) return;
        VkImageView out = sm->apply(F->cb, src, VkExtent2D{se.width, se.height}, aa);
        shadow_invalidate();
        if (out) {
            g_su.tex = r.tex;
            g_su.view = out;
        }
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "scene anti-aliasing: SMAA on the %ux%u scene (texture %llu)", se.width, se.height, static_cast<unsigned long long>(r.tex->id));
        }
        return;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        NXLOG_W(kTag, "scene anti-aliasing: no float scene input the size of the tone-map target (%ux%u): SMAA skipped", te.width, te.height);
        for (uint32_t i = 0; i < tried_n; ++i) NXLOG_W(kTag, "  tone-map input %u: %ux%u format %d", i, tried[i].w, tried[i].h, static_cast<int>(tried[i].f));
    }
}

void tonemap_for_draw(const ProgLayout& pl) {
    g_su.clear();
    g_su.stretch_armed = true;                                            // the frame's scene stretch comes after its tone map
    g_su.chain = nullptr;
    if (S.ncolor == 0 || !S.color_tex[0]) return;
    if (extras::temporal_wanted()) {
        ++g_tsr_frame;                                                    // one tone-map draw per frame: the next frame's jitter phase
        if (g_tsr_deferred) {                                             // last frame's stretch never came: its camera is stale
            extras::temporal_drop_camera();
            g_tsr_live = false;
        }
        if (temporal_deferred()) {
            g_tsr_deferred = true;                                        // the scene stretch runs it
            return;
        }
        temporal_for_draw(pl);
        return;
    }
    g_tsr_live = false;
    const extras::AaMode aa = extras::aa_mode();
    // SMAA runs when the scene is shown at its own size (an upscaled scene part keeps its upscaler; SMAA does not read a part of an image)
    if (aa != extras::AaMode::Off && (!extras::scene_upscale_wanted() || extras::upscale_scene_scale() >= 100)) {
        Texture* tgt = S.color_tex[0].get();
        if (tex_gpu(tgt)) smaa_scene_for_draw(pl, tex_image_extent(tgt, 0), aa);
    }
}

// A phone: what the frame rate counter's resolution line shows (gfx::Tuning::res_*): the scene part, the output, the upscaler.
static void publish_res(VkExtent2D scene, VkExtent2D out, int upscaler) {
    gfx::Tuning& tu = gfx::tuning();
    tu.res_scene_px.store((scene.width << 16) | (scene.height & 0xffff), std::memory_order_relaxed);
    tu.res_out_px.store((out.width << 16) | (out.height & 0xffff), std::memory_order_relaxed);
    tu.res_upscaler.store(upscaler, std::memory_order_relaxed);
}

// The game's scene stretch (game::SceneStretch, #571): the first such draw after the tone map that enlarges, its one 2D colour input no
// larger than its target. Copies of the same shader that shrink (the game's own reductions) and later copies are left alone.
// Nearest (#232): the input is sampled with the nearest sampler (clamp to edge), so the stretch shows the scene's own pixels.
// FSR 1 / NIS: the scene part of the input (scene_part) goes through the upscaler to the target's size, and the draw reads the result in an
// image sized so that its own lookup at uv * fraction lands on it. The input is display-referred colour in [0,1] (8-bit sRGB, read linear).
void scene_stretch_for_draw(const ProgLayout& pl) {
    g_su.clear();
    // [gfx] scene_upscale (sgsr / bicubic, live through scene_upscale_watch) also serves this stretch: under the game's dynamic resolution
    // (a scene percent below 100) the scene part is enlarged here, not at the composite (scene_sharpen_for_draw)
    extras::SgsrSpatial* cheap = extras::scene_cheap_upscaler(&device());
    const bool tsr = extras::temporal_wanted();
    if constexpr (facts::kPhone) {
        if (g_su.stretch_armed && !extras::scene_upscale_wanted() && !cheap && !tsr && S.ncolor > 0 && S.color_tex[0] && tex_gpu(S.color_tex[0].get())) {
            float fx = 1.0f, fy = 1.0f;   // no upscaler: the game's own stretch enlarges the scene part of a target-sized image
            extras::scene_fraction(&fx, &fy);
            const VkExtent3D e = tex_image_extent(S.color_tex[0].get(), 0);
            publish_res(VkExtent2D{static_cast<uint32_t>(std::lround(e.width * fx)), static_cast<uint32_t>(std::lround(e.height * fy))},
                        VkExtent2D{e.width, e.height}, 0);
        }
    }
    if (!g_su.stretch_armed || (!extras::scene_upscale_wanted() && !cheap && !tsr) || S.ncolor == 0 || !S.color_tex[0]) return;
    Texture* target = S.color_tex[0].get();
    if (!tex_gpu(target)) return;
    const VkExtent3D te = tex_image_extent(target, 0);    // image sizes: with a render scale the upscaler works on the real pixels
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D || b.image.depth) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex)) continue;
        const VkExtent3D se = tex_image_extent(r.tex, 0);
        if (se.width > te.width || se.height > te.height) return;          // a reduction, not the stretch
        if (g_su.chain && r.tex != g_su.chain) {                            // re-armed by a 1:1 copy: only an enlargement of that copy is the stretch
            g_su.stretch_armed = false;
            g_su.chain = nullptr;
            return;
        }
        g_su.chain = nullptr;
        g_su.stretch_armed = false;
        float fx = 1.0f, fy = 1.0f;
        extras::scene_fraction(&fx, &fy);
        if (tsr) {   // the temporal pass: the scene (part), straight to this target's size (temporal_deferred)
            VkExtent2D part{se.width, se.height}, out_tex{};
            const VkExtent2D out{te.width, te.height};
            scene_part(part, out, &part, &out_tex, r.tex);   // the part this frame drew (the published fraction can be the next frame's)
            if (part.width >= out.width && part.height >= out.height) {      // a 1:1 copy: a later stretch may still enlarge the scene
                g_su.stretch_armed = true;
                g_su.chain = target;                                          // ... read from this copy
                return;
            }
            g_tsr_stretch_frame = g_tsr_frame;
            if (!g_tsr_deferred) return;                                      // the tone map ran it this frame; from the next frame on, here
            end_pass();                                                       // compute work cannot run inside a render pass
            tex_ensure_current(r.tex, *F);
            tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
            VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
            // A phone: the result at the target's size, blitted 1:1 into the target with the game's draw skipped (as the spatial path below):
            // the draw reads its input at uv x fraction, which needed an image of out / fraction (at 15 % on a near-square phone screen: 1.4 GB)
            const bool blit = facts::kPhone && (out_tex.width > out.width || out_tex.height > out.height);
            VkImageView up = temporal_run(r.tex, src, part, out, blit ? out : out_tex, false);
            if constexpr (facts::kPhone) publish_res(part, out, up ? 3 : 0);
            if (blit) {
                if (up) {
                    memory_barrier(F->cb);
                    if (tex_blit_from(target, *F, extras::temporal_output_image(), VK_FORMAT_R16G16B16A16_SFLOAT, out)) {
                        memory_barrier(F->cb);
                        g_su.skip_draw = true;
                        static bool told_blit = false;
                        if (!told_blit) {
                            told_blit = true;
                            NXLOG_I(kTag, "temporal: %ux%u -> %ux%u written into the scene stretch's target (the draw is skipped)", part.width, part.height,
                                    out.width, out.height);
                        }
                    }
                }
                return;   // without the blit the game's own stretch draws
            }
            if (up) {
                g_su.tex = r.tex;
                g_su.view = up;
            }
            return;
        }
        if (!cheap && extras::upscale_mode() == extras::UpscaleMode::Nearest) {
            const bool smaller = se.width < te.width || se.height < te.height;
            // a scene_scale below 100 always puts the scene in the top-left of full-size targets, published fraction or not
            if (!smaller && fx >= 1.0f && fy >= 1.0f && extras::upscale_scene_scale() >= 100) return;   // a 1:1 copy
            g_su.near[g_su.near_n++] = r.tex;
            static Texture* told = nullptr;
            if (told != r.tex) {
                told = r.tex;
                NXLOG_I(kTag, "scene upscale: nearest on the scene stretch %s (%ux%u input, fraction %.3f x %.3f, scene_scale %d%%, target %ux%u)",
                        S.stage[kFragment]->hash.c_str(), se.width, se.height, fx, fy, extras::upscale_scene_scale(), te.width, te.height);
            }
            return;
        }
        // the scene is either a smaller image or the top-left part of a full-size one (scene_part)
        VkExtent2D part{se.width, se.height}, out_tex{};
        const VkExtent2D out{te.width, te.height};
        if (!scene_part(part, out, &part, &out_tex, r.tex) && se.width >= te.width && se.height >= te.height) {
            if constexpr (facts::kPhone) publish_res(VkExtent2D{se.width, se.height}, out, 0);
            static bool told_whole = false;
            if (!told_whole) {
                told_whole = true;
                NXLOG_I(kTag, "scene upscale: the scene stretch reads its whole %ux%u input at the target's size: nothing to upscale", se.width, se.height);
            }
            return;
        }
        if (part.width >= te.width && part.height >= te.height) return;
        if constexpr (facts::kPhone) publish_res(part, out, cheap ? (std::string(extras::scene_upscale_name()) == "sgsr" ? 1 : 2) : 0);
        extras::SpatialUpscaler* su = cheap ? static_cast<extras::SpatialUpscaler*>(cheap) : extras::scene_upscaler(&device(), part, out);
        if (!su) return;
        end_pass();                                                           // compute work cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
        VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
        if (!src) return;
        if constexpr (facts::kPhone) {
            // A phone: SGSR / bicubic enlarge the scene part straight into the stretch's target and the game's draw is skipped. The draw reads
            // its input at uv x fraction, so handing it the result needed an image of out / fraction (at 15 %: an RGBA16F image 6.7 times the screen per side, 1.4 GB,
            // made when the Resolution was lowered: a phone was killed for memory); this needs one of the target's size.
            if (cheap && (part.width < se.width || part.height < se.height)) {
                VkImageView up = cheap->upscale(F->cb, src, part, VkExtent2D{se.width, se.height}, out, false, 1.0f, 0.0f, out);
                shadow_invalidate();
                memory_barrier(F->cb);
                if (up && tex_blit_from(target, *F, cheap->image(), VK_FORMAT_R16G16B16A16_SFLOAT, out)) {
                    memory_barrier(F->cb);
                    g_su.skip_draw = true;
                    static bool told_phone = false;
                    if (!told_phone) {
                        told_phone = true;
                        NXLOG_I(kTag, "scene upscale: %s on the scene stretch %s, %ux%u -> %ux%u, written into its target (the draw is skipped)",
                                extras::scene_upscale_name(), S.stage[kFragment]->hash.c_str(), part.width, part.height, out.width, out.height);
                    }
                }
                return;   // without the blit the game's own stretch draws (bilinear)
            }
        }
        VkImageView up = su->upscale(F->cb, src, part, VkExtent2D{se.width, se.height}, out, false, 1.0f, extras::upscale_sharpness(), out_tex);
        shadow_invalidate();
        if (up) {
            g_su.tex = r.tex;
            g_su.view = up;
        }
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "scene upscale: %s on the scene stretch %s, %ux%u -> %ux%u", cheap ? extras::scene_upscale_name()
                    : extras::upscale_mode() == extras::UpscaleMode::Nis ? "NIS" : "FSR 1", S.stage[kFragment]->hash.c_str(), part.width, part.height, out.width, out.height);
        }
        return;
    }
}

// [gfx] scene_sharpen at the game's draw that enlarges the scene into the window texture (SceneStretch::into_window): its one 2D colour
// input, when smaller than the target, is enlarged by a bilinear blit to the target's size and sharpened with RCAS, and the draw reads that
// (it samples by normalized coordinates, so a draw into a corner of the target is served too). The input is display-referred (after the
// tone map). Key off, or an input not smaller: the draw is left as the game made it.
void scene_sharpen_for_draw(const ProgLayout& pl) {
    g_su.clear();
    extras::SgsrSpatial* cheap = extras::scene_cheap_upscaler(&device());   // [gfx] scene_upscale = sgsr / bicubic: one pass instead
    if (!cheap && extras::scene_sharpen() <= 0.0f) return;
    Texture* target = S.color_tex[0].get();
    if (!tex_gpu(target)) return;
    const VkExtent3D te = tex_image_extent(target, 0);
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D || b.image.depth) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex)) continue;
        const VkExtent3D se = tex_image_extent(r.tex, 0);
        if (se.width >= te.width || se.height >= te.height) return;      // not enlarged (ui_native off, or a render scale of 1)
        if (cheap) {
            end_pass();                                                   // compute work cannot run inside a render pass
            tex_ensure_current(r.tex, *F);
            tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
            VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
            if (!src) return;
            const VkExtent2D in{se.width, se.height}, out{te.width, te.height};
            VkImageView v = cheap->upscale(F->cb, src, in, in, out, false, 1.0f, 0.0f);
            shadow_invalidate();
            if (!v) return;
            g_su.tex = r.tex;
            g_su.view = v;
            static bool told_cheap = false;
            if (!told_cheap) {
                told_cheap = true;
                NXLOG_I(kTag, "scene upscale: %s, %ux%u -> %ux%u at %s ([gfx] scene_upscale)", extras::scene_upscale_name(), in.width, in.height, out.width,
                        out.height, S.stage[kFragment]->hash.c_str());
            }
            return;
        }
        extras::Fsr1Spatial* sh = extras::scene_sharpener(&device());
        if (!sh) return;
        end_pass();                                                       // a blit and compute work cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkExtent2D in{se.width, se.height}, out{te.width, te.height};
        VkImageView v = sh->sharpen_enlarged(F->cb, tex_image(r.tex), in, out, false, 1.0f, extras::scene_sharpen());
        shadow_invalidate();
        if (!v) return;
        g_su.tex = r.tex;
        g_su.view = v;
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "scene sharpen: %ux%u -> %ux%u, bilinear then RCAS (sharpness %.2f) at %s", in.width, in.height, out.width, out.height,
                    extras::scene_sharpen(), S.stage[kFragment]->hash.c_str());
        }
        return;
    }
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
