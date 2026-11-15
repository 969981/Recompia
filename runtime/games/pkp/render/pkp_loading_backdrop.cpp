// SPDX-License-Identifier: GPL-3.0-or-later
// The Ditto loading layout uses two black quads that end at the old 16:9 edges (#534).
#include "render/pkp_loading_backdrop.h"

#include <cmath>
#include <string_view>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "input/pkp_play.h"

namespace nxrt::games::pkp {
namespace {

using guest::rd;

constexpr uint64_t kQuadGeometry = 0x1f98b98;
constexpr uint64_t kPaneQuadReturn = 0x1f772ec;
constexpr size_t kSavedReturn = 8, kSavedDraw = 0x58;   // x29-relative LR and SP-relative caller x21 in the quad builder
constexpr size_t kDrawPane = 8;
constexpr size_t kWorldY = 13;
constexpr float kAuthoredHalfWidth = 960.f, kAuthoredHalfHeight = 540.f;

bool named(uint64_t pane, std::string_view name) {
    if (!pane || rd<uint16_t>(pane + layout::kPaneNameLen) != name.size()) return false;
    const uint64_t text = rd<uint64_t>(pane + layout::kPaneName);
    return text && std::string_view(reinterpret_cast<const char*>(text), name.size()) == name;
}

int extend_backdrop(::Ctx* c, uint64_t addr, void*) {
    // This caller supplies an authored pane. The quad builder saved the caller's draw object (x21) on entry.
    const uint64_t base = addr - kQuadGeometry;
    if (rd<uint64_t>(c->x[29] + kSavedReturn) != base + kPaneQuadReturn) return 0;
    float& half_height = c->v[1].f32[0];
    float& local_y = c->v[3].f32[0];
    if (c->v[0].f32[0] != kAuthoredHalfWidth || local_y != 0.f) return 0;

    const uint64_t draw = rd<uint64_t>(c->x[31] + kSavedDraw);
    const uint64_t pane = rd<uint64_t>(draw + kDrawPane);
    const bool top = half_height == 120.f && named(pane, "0000_prt_square_w_0001");
    const bool bottom = half_height == 230.f && named(pane, "0000_prt_square_w_0000");
    if (!top && !bottom) return 0;
    if (!named(rd<uint64_t>(pane + layout::kPaneParent), "Null_ani_all")) return 0;

    const uint64_t world = rd<uint64_t>(pane + layout::kPaneWorld);
    if (!world) return 0;
    const float* matrix = reinterpret_cast<const float*>(world);
    if (matrix[0] != 1.f || matrix[1] != 0.f || matrix[4] != 0.f || matrix[5] != 1.f || matrix[12] != 0.f) return 0;
    const float direction = top ? 1.f : -1.f;
    const float outer_edge = direction * matrix[kWorldY] + half_height;
    if (!(std::fabs(outer_edge - kAuthoredHalfHeight) <= 0.5f)) return 0;

    const float canvas_half_height = play_ui_canvas_height() * 0.5f;
    const float extra_half = (canvas_half_height - outer_edge) * 0.5f;
    // Move only the outer edge. These registers belong to this draw; the pane and its matrix stay reusable.
    half_height += extra_half;
    local_y += direction * extra_half;
    return 0;
}

void install(const FeatureInstall& in) {
    if (play_ui_canvas_height() > kAuthoredHalfHeight * 2.f)
        in.hooks.add_mid(in.base + kQuadGeometry, &extend_backdrop);
}

}  // namespace

const Feature kLoadingBackdropFeature = {.name = "loading_backdrop", .install = &install};

}  // namespace nxrt::games::pkp
