// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's curtain (pkp_coop_internal.h): a black cover over the game's picture, with one line of text, while a
// member joins or leaves the host's world. What it hides is the game's own Link Play travel (the PC, the transfer, the landing) and
// the warp next to the host, which seamless co-op makes without the game's menus. The session part (pkp_coop_session.cpp) says when;
// this part only fades and draws. It is the co-op feature's overlay layer, drawn over the picture and under the overlay's own menu.
// A second, dimmer line (hint) is set by the session part where a dialog of the game's that waits for A may sit under the cover.
#include "core/pkp_coop_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>

#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::curtain {

namespace {

constexpr double kFade = 0.3;   // seconds to fade in or out
constexpr double kNever = std::numeric_limits<double>::infinity();

struct State {
    std::mutex m;
    double cover_at = -kNever, lift_at = -kNever;   // lift_at: when the fade out starts (kNever: held)
    std::string text, hint;
};
State& st() {
    static State s;
    return s;
}

// The cover's opacity at now, 0..1 (caller holds the lock).
double alpha(const State& s, double now) {
    if (now < s.cover_at) return 0.0;
    const double in = std::min(1.0, (now - s.cover_at) / kFade);
    const double out = now < s.lift_at ? 1.0 : std::max(0.0, 1.0 - (now - s.lift_at) / kFade);
    return std::min(in, out);
}

uint32_t rgba(uint32_t rgb, double a) { return rgb << 8 | static_cast<uint32_t>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0)); }

}  // namespace

void cover(const char* text) {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.m);
    const double now = qpc::seconds();
    const double a = alpha(s, now);
    if (a <= 0.0) s.cover_at = now;
    else if (s.lift_at != kNever) s.cover_at = now - a * kFade;   // fading out: back in from the opacity it has now
    s.lift_at = kNever;
    s.text = text;
    s.hint.clear();
}

void hint(const char* text) {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.m);
    s.hint = text;
}

void lift(double after) {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.m);
    if (s.lift_at != kNever) return;
    s.lift_at = qpc::seconds() + after;
}

bool covered() {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.m);
    return alpha(s, qpc::seconds()) >= 1.0;
}

bool overlay_active() {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.m);
    return alpha(s, qpc::seconds()) > 0.0;
}

void overlay_draw(gfx::OverlayCanvas& c) {
    State& s = st();
    double a, up;
    std::string text, hint;
    {
        std::lock_guard<std::mutex> lock(s.m);
        const double now = qpc::seconds();
        a = alpha(s, now);
        up = now - s.cover_at;
        text = s.text;
        hint = s.hint;
    }
    if (a <= 0.0) return;
    const float w = c.width(), h = c.height(), lh = c.line_height();
    c.fill(0.f, 0.f, w, h, rgba(0x000000, a));
    const int dots = static_cast<int>(up * 2.0) % 4;
    const std::string shown = text + std::string(static_cast<size_t>(dots), '.');
    const float tw = c.text_width((text + "...").c_str());
    c.text((w - tw) * 0.5f, h * 0.5f - lh, rgba(0xffffff, a), shown.c_str());
    if (!hint.empty()) c.text((w - c.text_width(hint.c_str())) * 0.5f, h * 0.5f + lh * 0.5f, rgba(0xa0a0a0, a), hint.c_str());
}

void reset() {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.m);
    s.cover_at = s.lift_at = -kNever;
    s.text.clear();
    s.hint.clear();
}

}  // namespace nxrt::games::pkp::coop::curtain
