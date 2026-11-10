// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the Graphics tab (HDR output, quality preset, picture, GPU and shaders, colour).
#include "play_ui_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::play {
namespace ui {

constexpr const char* kAnisoNames[] = {"Game default", "2x", "4x", "8x", "16x"};
constexpr int kAnisoValues[] = {0, 2, 4, 8, 16};
constexpr int kAnisoCount = static_cast<int>(sizeof kAnisoValues / sizeof kAnisoValues[0]);
constexpr const char* kShadowNames[] = {"Default", "High (2x)", "Very High (3x)", "Ultra (4x)", "Extreme (8x)"};   // indexed by extras::ShadowQuality
constexpr const char* kAstcValues[] = {"fast", "high", "uncompressed"};
constexpr const char* kAstcNames[] = {"Fast", "High (sharper, built in the background)", "Uncompressed (lots of video memory)"};
constexpr int kAstcCount = static_cast<int>(sizeof kAstcValues / sizeof kAstcValues[0]);
constexpr const char* kHdrModeNames[] = {"Off", "Auto", "On", "Native"};
constexpr const char* kHdrFormatNames[] = {"Auto", "HDR10", "scRGB"};

// A nits field where 0 means "auto" (shown as "auto"). Other values are clamped into lo..hi when the field is left, so the slider can be
// dragged through the gap below lo while it is held.
bool auto_nits_slider(const char* label, double* v, int lo, int hi) {
    int x = static_cast<int>(std::lround(*v));
    bool changed = ImGui::SliderInt(label, &x, 0, hi, x <= 0 ? "auto" : "%d nits");
    if (ImGui::IsItemDeactivated()) {
        const int c = x <= 0 ? 0 : std::clamp(x, lo, hi);
        changed = changed || c != x;
        x = c;
    }
    if (changed) *v = x;
    return changed;
}

void hdr_group(Page& p) {
    extras::HdrSettings& h = p.edit.extras.hdr;
    ImGui::SeparatorText("HDR output");

    const int mode = static_cast<int>(h.mode);
    const bool mode_open = ImGui::BeginCombo("Mode##hdr_mode", kHdrModeNames[mode]);
    tip("Off keeps the SDR picture, Auto uses HDR when the display and Windows offer it, On runs the HDR pipeline even without an HDR display. "
        "Native is On with the levels the game declares for an HDR TV on Switch 2.");
    if (mode_open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kHdrModeNames[i], i == mode) && i != mode) {
                h.mode = static_cast<extras::HdrMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    const int format = static_cast<int>(h.format);
    const bool format_open = ImGui::BeginCombo("Format##hdr_format", kHdrFormatNames[format]);
    tip("The signal sent to the display: HDR10, or scRGB; Auto picks HDR10 when the display offers it and scRGB otherwise.");
    if (format_open) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kHdrFormatNames[i], i == format) && i != format) {
                h.format = static_cast<extras::HdrFormat>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    if (auto_nits_slider("Peak brightness##hdr_peak", &h.peak_nits, 200, 10000)) mark(p);
    tip("How bright the brightest highlights get, in nits; auto reads the peak brightness the display reports.");
    if (auto_nits_slider("Paper white##hdr_paper", &h.paper_white_nits, 80, 500)) mark(p);
    tip("The brightness of ordinary white in the game, in nits; auto follows Windows' SDR content brightness when HDR is on there, else 203.");
    if (slider("Highlight boost", &h.highlight_boost, 0.0, 2.0, "%.2f")) mark(p);
    tip("0 keeps the game's SDR look up to its own white, 1 lets highlights reach the display's peak, 2 pushes them further.");

    const extras::HdrState hs = extras::hdr_state();
    ImGui::Text("HDR scene: %s, swapchain: %s, paper white %.0f nits, peak %.0f nits, peak ratio %.2f, display reports HDR: %s",
                hs.scene_hdr ? "on" : "off", extras::output_kind_name(hs.output), static_cast<double>(hs.paper_white_nits),
                static_cast<double>(hs.peak_nits), static_cast<double>(hs.peak_ratio), yes_no(hs.display_hdr));
}

// ---- Graphics -----------------------------------------------------------------------------------------------------------------------

// The game's quality preset (kDisplayOptQuality, the presets of play_types.h quality_presets()): picking one writes its values (texture
// LOD bias, anisotropy, shadow quality and anti-aliasing; a device preset also the render scale and the frame limit); changing one of those afterwards makes it Custom
// (quality_preset_edited).

// What a preset sets, in plain words, from the values the game's hook writes.
std::string quality_preset_summary(const QualityPresetInfo& q, const GamePlay* gp, const Settings& now) {
    std::string t = q.about;
    if (!gp || !gp->quality_preset) return t;
    Settings sv = now;
    if (!gp->quality_preset(q.id, sv)) return t;
    const DisplaySettings& v = sv.display;
    char buf[160];
    t += "\n";
    if (q.sets_frame) {
        if (v.render_scale <= 0) {
            t += "Render scale: automatic (the window's size). ";
        } else {
            std::snprintf(buf, sizeof buf, "Render scale: %d%% (%d x %d). ", v.render_scale, 1920 * v.render_scale / 100, 1080 * v.render_scale / 100);
            t += buf;
        }
        if (v.fps_cap < 0) {
            t += "Frame limit: the display's refresh rate. ";
        } else if (v.fps_cap == 0) {
            t += "Frame limit: none. ";
        } else {
            std::snprintf(buf, sizeof buf, "Frame limit: %d fps. ", v.fps_cap);
            t += buf;
        }
    }
    if (v.anisotropy > 0) {
        std::snprintf(buf, sizeof buf, "Texture filtering: %dx. ", v.anisotropy);
        t += buf;
    } else {
        t += "Texture filtering: the game's. ";
    }
    {
        static const char* const kShadowSummary[] = {"the game's", "2x", "3x", "4x", "8x"};
        t += std::string("Shadows: ") + kShadowSummary[static_cast<int>(v.shadow_quality)] + ". ";
    }
    {
        static const char* const kAaSummary[] = {"off", "SMAA Low", "SMAA Medium", "SMAA High", "SMAA Ultra"};
        t += std::string("Anti-aliasing: ") + kAaSummary[static_cast<int>(sv.extras.upscale.aa)] + ". ";
    }
    t += v.lod_bias < 0 ? "Textures: sharper." : v.lod_bias > 0 ? "Textures: softer." : "Textures: the game's sharpness.";
    if (q.sets_frame) t += "\nThe render scale applies on restart.";
    return t;
}

void quality_preset_combo(Page& p, const GamePlay* gp) {
    DisplaySettings& d = p.edit.display;
    int count = 0;
    const QualityPresetInfo* qs = quality_presets(&count);
    int sel = count - 1;   // custom
    for (int i = 0; i < count; ++i)
        if (d.quality_preset == qs[i].id) sel = i;
    if (ImGui::BeginCombo("Quality preset", qs[sel].name)) {
        for (int i = 0; i < count - 1; ++i) {
            if (ImGui::Selectable(qs[i].name, i == sel) && i != sel && gp->quality_preset && gp->quality_preset(qs[i].id, p.edit)) mark(p);
            if (ImGui::IsItemHovered()) {
                const std::string t = quality_preset_summary(qs[i], gp, p.edit);
                ImGui::SetItemTooltip("%s", t.c_str());
            }
        }
        ImGui::EndCombo();
    }
    tip("Sets several picture settings together. Switch 2 is the console's picture; High and Ultra are sharper and reach farther. The device "
        "presets also pick a render scale and a frame limit for that kind of PC. Changing one of those settings afterwards makes it Custom.");
}

void picture_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    const GamePlay* gp = game_play();
    const unsigned opts = gp ? gp->display_options : 0u;
    ImGui::SeparatorText("Picture");

    if (opts & kDisplayOptAspect) {
        const int aspect_sel = aspect_index(d.aspect, p.aspect_custom);
        if (ImGui::BeginCombo("Aspect ratio", kAspectNames[aspect_sel])) {
            for (int i = 0; i < kAspectNameCount; ++i) {
                if (ImGui::Selectable(kAspectNames[i], i == aspect_sel) && i != aspect_sel) {
                    if (i == kAspectCustom) {
                        p.aspect_custom = true;
                        if (d.aspect <= 0.0) d.aspect = kAspectValues[1];   // Custom starts at 16:9
                    } else {
                        p.aspect_custom = false;
                        d.aspect = kAspectValues[i];                       // Auto is index 0, value 0
                    }
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        restart_badge();
        if (aspect_sel == kAspectCustom && slider("Custom aspect ratio", &d.aspect, 1.0, 5.0, "%.2f")) mark(p);
    }

    ImGui::TextUnformatted("Fixed render size (0 = follow the window)");
    ImGui::SetNextItemWidth(px(120.0f));
    if (render_size_field("##render_w", &d.render_width)) mark(p);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(120.0f));
    if (render_size_field("##render_h", &d.render_height)) mark(p);
    restart_badge();
    tip("Renders at exactly this size instead of the window size times the render scale (Display page).");

    if (opts & kDisplayOptFov) {
        if (slider("Field of view offset (degrees)", &d.fov_offset, -30.0, 40.0, "%.1f")) mark(p);
        tip("Degrees added to the vertical field of view of every game camera. 0 is the game's own.");
    }
    if (ImGui::Checkbox("Dynamic resolution", &d.dynamic_resolution)) mark(p);
    tip("The game's own dynamic resolution: it lowers the render size when the GPU is late. Off keeps the full size.");

    const bool quality = (opts & kDisplayOptQuality) != 0;
    if (quality) quality_preset_combo(p, gp);
    auto custom = [&](QualityField f) {
        if (quality) quality_preset_edited(d, f);
        mark(p);
    };
    int aniso_sel = 0;
    for (int i = 0; i < kAnisoCount; ++i)
        if (kAnisoValues[i] == d.anisotropy) aniso_sel = i;
    if (ImGui::BeginCombo("Texture anisotropy", kAnisoNames[aniso_sel])) {
        for (int i = 0; i < kAnisoCount; ++i) {
            if (ImGui::Selectable(kAnisoNames[i], i == aniso_sel) && i != aniso_sel) {
                d.anisotropy = kAnisoValues[i];
                custom(QualityField::Anisotropy);
            }
        }
        ImGui::EndCombo();
    }
    tip("Sharper textures at a slant. Game default keeps what the game asks for; the other entries are a minimum on every filtering sampler.");
    {   // shadow map resolution: the shadow maps are remade at the new size on the next frame
        const int sq = static_cast<int>(d.shadow_quality);
        if (ImGui::BeginCombo("Shadow quality##shadows_quality", kShadowNames[sq])) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(kShadowNames[i], i == sq) && i != sq) {
                    d.shadow_quality = static_cast<gfx::ShadowQuality>(i);
                    custom(QualityField::Shadows);
                }
            }
            ImGui::EndCombo();
        }
        tip("Resolution of the game's shadow maps. Default: as the game draws them. High: 2x, Very High: 3x, Ultra: 4x and Extreme: 8x larger per side, "
            "which makes shadow edges sharper and costs video memory and speed (Ultra and Extreme a lot). Applies at once.");
    }
    if (slider("Texture LOD bias", &d.lod_bias, -2.0, 2.0, "%.2f")) custom(QualityField::LodBias);
    tip("Negative values pick sharper texture levels, positive values softer ones.");
    if (opts & kDisplayOptBloom) {
        if (ImGui::Checkbox("Bloom", &d.bloom)) mark(p);
    }
    if (opts & kDisplayOptChromatic) {
        if (ImGui::Checkbox("Chromatic aberration", &d.chromatic_aberration)) mark(p);
    }
}

void gpu_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    ImGui::SeparatorText("GPU and shaders");
    int astc_sel = 1;
    for (int i = 0; i < kAstcCount; ++i)
        if (d.astc_quality == kAstcValues[i]) astc_sel = i;
    if (ImGui::BeginCombo("Texture compression quality", kAstcNames[astc_sel])) {
        for (int i = 0; i < kAstcCount; ++i) {
            if (ImGui::Selectable(kAstcNames[i], i == astc_sel) && i != astc_sel) {
                d.astc_quality = kAstcValues[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("The game's textures are ASTC, which this GPU cannot sample, so they are converted. Fast: the quick BC7 conversion. High: the same at first, then a background thread "
        "re-encodes each texture slowly into the texture cache, so later runs are sharper (more disk space, no frame cost). Uncompressed: RGBA8, sharpest, 4 times the video memory "
        "of BC7. [gfx] astc_quality in the ini wins over this.");
    if (ImGui::Checkbox("Faster GPU memory (uses more video memory)", &d.fast_gpu_memory)) mark(p);
    tip("Keeps device-local copies of the memory the draws read: faster, costs up to 2 GiB of video memory. [nvn] pool_mirror in the ini wins over this.");
    restart_badge();
    if (ImGui::Checkbox("Fewer GPU pipeline waits", &d.fewer_gpu_waits)) mark(p);
    tip("No full barrier after render passes that only wrote their own targets. [nvn] pass_barriers in the ini wins over this.");
    restart_badge();
    if (ImGui::Checkbox("Prepare shaders ahead (pipeline prewarm)", &d.shader_prewarm)) mark(p);
    tip("Builds the pipelines that earlier runs recorded when the game loads a program, on background threads, so draws do not stall on a first compile. [nvn] prewarm in the ini wins over this.");
    restart_badge();
    if (const int left = gfx::shader_backlog(); left > 0) ImGui::TextDisabled("Preparing shaders: %d left", left);
    else ImGui::TextDisabled("No shaders waiting to be prepared");
}

// Colour controls of the present step (nxrt/extras.h PictureSettings). They show live while dragged; Apply and save keeps them.
void color_group(Page& p) {
    extras::PictureSettings& c = p.edit.extras.picture;
    ImGui::SeparatorText("Colour");
    bool changed = false;
    if (ImGui::Button("Neutral##pic_neutral")) changed = extras::picture_preset("neutral", &c);
    tip("The game's own picture, untouched.");
    ImGui::SameLine();
    if (ImGui::Button("Vivid##pic_vivid")) changed = extras::picture_preset("vivid", &c);
    tip("A modest boost: contrast 1.10, saturation 1.15, black level -0.03.");
    changed |= slider("Contrast##pic_contrast", &c.contrast, 0.5, 1.5, "%.2f");
    tip("Spreads the tones away from mid grey; 1 is the game's own.");
    changed |= slider("Saturation##pic_saturation", &c.saturation, 0.0, 2.0, "%.2f");
    tip("Colour intensity; 0 is grey, 1 is the game's own.");
    changed |= slider("Black level##pic_black", &c.black, -0.5, 0.25, "%.3f");
    tip("Below 0 pulls the darkest tones down to black (against a hazy, washed-out look); above 0 lifts them. White stays. 0 is the game's own.");
    changed |= slider("Gamma##pic_gamma", &c.gamma, 0.5, 2.0, "%.2f");
    tip("Mid-tone brightness: above 1 brightens, below 1 darkens. Black and white stay. 1 is the game's own.");
    if (changed) {
        extras::set_picture_live(c);
        mark(p);
    }
    ImGui::TextDisabled(c.neutral() ? "Neutral: the picture is presented as the game drew it." : "Applies live; Apply and save keeps it.");
}

void graphics_page(Page& p) {
    hdr_group(p);
    picture_group(p);
    color_group(p);
    gpu_group(p);
}


}  // namespace ui
}  // namespace nxrt::play
