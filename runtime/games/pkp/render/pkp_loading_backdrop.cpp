// SPDX-License-Identifier: GPL-3.0-or-later
// What covers the picture while the game loads ends at or near the old 16:9 edges (#534): the loading screen layout's full-width quads
// (two black ones over the strip's scenery, and the strip's sky and ground under it) and the black quad the game fades the picture
// with before and after it. On a wider or taller canvas the scene showed beside them. The game sizes the layout to the canvas: on one
// wider than 1920 and taller than 16:9 (a phone's screen-sized targets) every pane is the authored one times the canvas width / 1920, so
// the authored sizes and positions are matched at that scale.
#include "render/pkp_loading_backdrop.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "input/pkp_play.h"

namespace nxrt::games::pkp {
namespace {

using guest::rd;

using layout::kDrawPane;
using layout::kQuadGeometry;
constexpr size_t kWorldY = 13;
constexpr float kAuthoredHalfWidth = 960.f, kAuthoredHalfHeight = 540.f;

// The game's scale of the authored 1920x1080 layout on this canvas (it fits the canvas), and a match at it: the game rounds each scaled
// size to whole pixels (a scaled half height such as 668.25 is drawn 668.5), so a half size is within half a pixel of it.
float layout_scale() {
    return std::min(play_ui_canvas_width() / (kAuthoredHalfWidth * 2.f), play_ui_canvas_height() / (kAuthoredHalfHeight * 2.f));
}
bool same(float v, float authored, float scale) { return std::fabs(v - authored * scale) <= 0.51f; }

bool named(uint64_t pane, std::string_view name) {
    if (!pane || rd<uint16_t>(pane + layout::kPaneNameLen) != name.size()) return false;
    const uint64_t text = rd<uint64_t>(pane + layout::kPaneName);
    return text && std::string_view(reinterpret_cast<const char*>(text), name.size()) == name;
}

// The layout's full-width panes, all children of Null_ani_all and centred on the canvas. The two black ones lie over the strip's
// scenery and end at the canvas's top and bottom edge. The strip's sky and ground lie under the scenery and carry names other layouts
// use too, so they count only under the pane the black ones were last drawn under.
struct Backdrop {
    std::string_view name;
    float half_height, world_y;
    float outward;   // +1: the top edge is the canvas's, -1: the bottom edge, 0: the strip's own
};
constexpr Backdrop kBackdrops[] = {
    {"0000_prt_square_w_0001", 120.f, 420.f, 1.f},
    {"0000_prt_square_w_0000", 230.f, -310.f, -1.f},
    {"0000_prt_square_w", 540.f, 0.f, 0.f},
    {"0000_prt_dummy_0", 180.f, -220.f, 0.f},
};
std::atomic<uint64_t> g_group{0};

// The fade: one black quad whose alpha the game animates, centred on the canvas and authored a tenth larger than it (1056 x 594 half
// size), so it ends inside a canvas more than a tenth wider or taller than 16:9 (2112 wide: about 1.96:1).
constexpr std::string_view kFadeName = "000_prt_square_k";
constexpr float kFadeHalfWidth = 1056.f, kFadeHalfHeight = 594.f;

// The world matrix of a pane that is drawn unscaled, unrotated and centred on the canvas's width; null for any other pane.
const float* centred_world(uint64_t pane) {
    const uint64_t world = rd<uint64_t>(pane + layout::kPaneWorld);
    if (!world) return nullptr;
    const float* matrix = reinterpret_cast<const float*>(world);
    if (matrix[0] != 1.f || matrix[1] != 0.f || matrix[4] != 0.f || matrix[5] != 1.f || matrix[12] != 0.f) return nullptr;
    return matrix;
}

int extend_backdrop(::Ctx* c, uint64_t addr, void*) {
    // This caller supplies an authored pane. The quad builder saved the caller's draw object (x21) on entry.
    const uint64_t base = addr - kQuadGeometry;
    if (rd<uint64_t>(c->x[29] + layout::kQuadReturn) != base + layout::kPictureQuadReturn) return 0;
    float& half_width = c->v[0].f32[0];
    float& half_height = c->v[1].f32[0];
    const float local_x = c->v[2].f32[0];
    float& local_y = c->v[3].f32[0];
    if (local_y != 0.f) return 0;
    const float scale = layout_scale();
    const bool fade = same(half_width, kFadeHalfWidth, scale) && same(half_height, kFadeHalfHeight, scale);
    if (!fade && !same(half_width, kAuthoredHalfWidth, scale)) return 0;

    const uint64_t draw = rd<uint64_t>(c->x[31] + layout::kQuadPictureDraw);
    const uint64_t pane = rd<uint64_t>(draw + kDrawPane);
    // These registers belong to this draw; the pane and its matrix stay reusable.
    if (fade) {
        if (local_x != 0.f || !named(pane, kFadeName)) return 0;
        const float* matrix = centred_world(pane);
        if (!matrix || matrix[kWorldY] != 0.f) return 0;
        // the same tenth over the canvas it has; the canvas is never smaller than the authored one
        half_width = play_ui_canvas_width() * 0.5f * (kFadeHalfWidth / kAuthoredHalfWidth);
        half_height = play_ui_canvas_height() * 0.5f * (kFadeHalfHeight / kAuthoredHalfHeight);
        return 0;
    }
    const Backdrop* quad = nullptr;
    for (const Backdrop& b : kBackdrops)
        if (same(half_height, b.half_height, scale) && named(pane, b.name)) quad = &b;
    if (!quad) return 0;
    const uint64_t group = rd<uint64_t>(pane + layout::kPaneParent);
    if (!named(group, "Null_ani_all")) return 0;

    const float* matrix = centred_world(pane);
    if (!matrix || !(std::fabs(matrix[kWorldY] - quad->world_y * scale) <= 0.5f * scale)) return 0;
    const bool black = quad->outward != 0.f;
    if (black) g_group.store(group, std::memory_order_relaxed);
    else if (group != g_group.load(std::memory_order_relaxed)) return 0;

    const float canvas_half_height = play_ui_canvas_height() * 0.5f;
    if (black && canvas_half_height > kAuthoredHalfHeight * scale) {   // taller canvas: move only the outer edge
        const float outer_edge = quad->outward * matrix[kWorldY] + half_height;
        const float extra_half = (canvas_half_height - outer_edge) * 0.5f;
        half_height += extra_half;
        local_y += quad->outward * extra_half;
    }
    // Wider canvas: the quad is centred on it (world x 0 above, local x 0), so both side edges move out and the centre stays.
    const float canvas_half_width = play_ui_canvas_width() * 0.5f;
    if (canvas_half_width > kAuthoredHalfWidth * scale && local_x == 0.f) half_width = canvas_half_width;
    return 0;
}

void install(const FeatureInstall& in) {
    if (play_ui_canvas_width() > kAuthoredHalfWidth * 2.f || play_ui_canvas_height() > kAuthoredHalfHeight * 2.f)
        in.hooks.add_mid(in.base + kQuadGeometry, &extend_backdrop);
}

}  // namespace

const Feature kLoadingBackdropFeature = {.name = "loading_backdrop", .install = &install};

}  // namespace nxrt::games::pkp
