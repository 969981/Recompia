// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: text spacing on displays wider than 16:9 (#176; split out of pkp_play.cpp).
#include "input/pkp_play_internal.h"

#include <cstdint>

#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp::play_detail {

// #176: wide displays squeezed text (letters running into each other, words merging). The UI canvas follows the game targets (3840x1080 at
// 32:9), and the game scales a text box's font size by the canvas width (0.28125 at 1920 wide, 0.5625 at 3840) but its character spacing by
// that factor squared (-2.5, -10). The text renderer then normalises the font size to the same glyph scale at every shape (and the spacing
// with it: -5 at 16:9, -10 at 32:9), so on a wider canvas the negative spacing grows by the widening and the glyphs overlap. Fixed where
// every text box's settings are copied out for layout, fitting and drawing (0x1f77930, mid hook on the spacing store 0x1f779d0, s0 = the
// spacing): divided by the widening (display aspect / 16:9). The text writer's style (0x1f8775c(writer, style), copied into the writer on every
// call) carries the same spacing for measuring and fitting (writer + 0x38ec, read by 0x1f8e960 and the string width 0x1f515a0); left at the
// raw value, the fit sees the text as narrower than it draws and the last letters of right-aligned text fall outside their box. That copy is
// corrected too, after its stores (0x1f877b0, x19 = the writer). Neither the text box nor the style is changed, so nothing compounds.
constexpr uint64_t kWriterSpacing = 0x38ec;
std::atomic<uint64_t> g_text_fixes{0};
double text_widen() { return wide().aspect / kAspect169; }
int hook_text_spacing(::Ctx* c, uint64_t, void*) {
    const double widen = text_widen();
    if (!(widen > 1.02)) return 0;
    const float sp = c->v[0].f32[0];
    c->v[0].f32[0] = static_cast<float>(sp / widen);
    if (g_text_fixes.fetch_add(1, std::memory_order_relaxed) == 0) NXLOG_I(kTag, "wide text: character spacing %.3f -> %.3f (widening %.3f)", sp, c->v[0].f32[0], widen);
    return 0;
}
int hook_text_writer_style(::Ctx* c, uint64_t, void*) {
    const double widen = text_widen();
    const uint64_t a = c->x[19] + kWriterSpacing;
    if (!(widen > 1.02) || !c->x[19] || !mem::is_mapped(a, 4)) return 0;
    wr<float>(a, static_cast<float>(rd<float>(a) / widen));
    return 0;
}

}  // namespace nxrt::games::pkp::play_detail
