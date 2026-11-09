// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: display aspects other than 16:9 (ultrawide, 16:10) for the scene cameras and the UI layout camera (split out
// of pkp_play.cpp).
#include "pkp_play_internal.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp::play_detail {

// ---- aspect ratio (ultrawide, 16:10) ----
// KTGL's camera class (init 0x1f12368: +16 eye, +32 target, +48 up, +320 near, +324 far, +328 vertical FOV in radians, +332 aspect, +340 zoom,
// +344 bit 0 orthographic; +128 view rows, +192 projection rows, +256 view-projection rows = P0 * V.x + P1 * V.y + P2 * V.z + P3 * V.w per view
// row) builds its projection in 0x1f12114 from the FOV and the aspect: P[0][0] = cot(fov / 2) / aspect, P[1][1] = cot(fov / 2). Every game
// camera is created with the aspect 1.7778 written as a constant. The scene's cameras (the camera manager's block [0x3483e18] + 18864 + 608 * n,
// or the view's own camera) reach the renderer through 0x8cbe64, which hands the camera's view-projection to the render view
// (0x18d3868(view slot, camera + 256, ...)): the mid hook there (0x8cc124, x20 = the camera) widens a 16:9 camera to the display's aspect.
// Wider than 16:9 (Hor+): the vertical FOV stays, P[0][0] = P[1][1] / aspect and +332 = aspect, so the game's own later rebuilds stay wide
// (and the mid hook in the builder, 0x1f12130 with x0 = the camera, puts the aspect back into a known scene camera whenever the game resets it)
// and everything that culls from the camera sees the wider frustum. Narrower than 16:9: the horizontal reach stays and the view grows above and
// below (P[1][1] = P[0][0] * aspect; +332 is left alone, so this one is matrix-only: draws that use the 16:9 projection then sit at the
// wrong height, #73; by default ([pkp] taller_fov on) it is made through the camera's FOV and aspect instead, see taller_fields; with it off,
// auto keeps 16:9 there).
// The UI's screen-layout camera (0x1fbab60(out, eye, int size[2]): an orthographic camera whose P[0][0] / P[1][1] are 2 / w, 2 / h of the size,
// origin at the canvas centre) gets a size of the display's shape when it is asked for the 1920x1080 canvas (mid hook 0x1fbab98, x2 the size):
// the 16:9 layouts stay 16:9, centred over the wider (or taller) scene.
constexpr size_t kCamFov = 328, kCamAspect = 332, kCamView = 128, kCamProj = 192, kCamViewProj = 256;

Wide& wide() {
    static Wide w;
    return w;
}
bool close_to(double a, double b) { return std::fabs(a - b) < 0.02; }
bool wide_on() { return !close_to(wide().aspect, kAspect169); }

bool known_cam(uint64_t cam) {
    for (auto& c : wide().cams)
        if (c.load(std::memory_order_relaxed) == cam) return true;
    return false;
}
void remember_cam(uint64_t cam) {
    if (known_cam(cam)) return;
    for (auto& c : wide().cams) {
        uint64_t z = 0;
        if (c.compare_exchange_strong(z, cam, std::memory_order_relaxed)) return;
    }
}

// Widens (or heightens) one camera's projection and view-projection from 16:9 to the display's aspect. Idempotent: a camera already at the
// display's shape, or at any shape other than 16:9, is left alone.
void fix_camera(uint64_t cam) {
    Wide& w = wide();
    float* P = reinterpret_cast<float*>(cam + kCamProj);
    const float p00 = P[0], p11 = P[5];
    if (!(std::fabs(p00) > 1e-6f) || !(std::fabs(p11) > 1e-6f)) return;
    const double own = std::fabs(p11 / p00);
    if (close_to(own, w.aspect) || !close_to(own, kAspect169)) return;
    const bool wider = w.aspect > kAspect169;
    const int lane = wider ? 0 : 1;
    if (wider) P[0] = std::copysign(static_cast<float>(std::fabs(p11) / w.aspect), p00);
    else P[5] = std::copysign(static_cast<float>(std::fabs(p00) * w.aspect), p11);
    const float* V = reinterpret_cast<const float*>(cam + kCamView);
    float* VP = reinterpret_cast<float*>(cam + kCamViewProj);
    for (int i = 0; i < 4; ++i) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += P[4 * k + lane] * V[4 * i + k];
        VP[4 * i + lane] = sum;
    }
    if (wider && close_to(rd<float>(cam + kCamAspect), kAspect169)) wr<float>(cam + kCamAspect, static_cast<float>(w.aspect));
    w.cam_fixes.fetch_add(1, std::memory_order_relaxed);
    if (!w.fov_logged.exchange(true)) {
        const double fov = rd<float>(cam + kCamFov);
        const double kDeg = 180.0 / 3.14159265358979323846;
        const double vt = 2.0 * std::atan(1.0 / std::fabs(P[5])), ht = 2.0 * std::atan(1.0 / std::fabs(P[0]));
        NXLOG_I(kTag, "aspect %.4f: scene camera 0x%llx widened, game FOV %.1f deg vertical; now %.1f deg vertical x %.1f deg horizontal (16:9 was %.1f "
                "horizontal)", w.aspect, static_cast<unsigned long long>(cam), fov * kDeg, vt * kDeg, ht * kDeg,
                2.0 * std::atan(std::tan(fov * 0.5) * kAspect169) * kDeg);
    }
}

void taller_fields(uint64_t cam);   // [pkp] taller_fov, below

int hook_cam_submit(::Ctx* c, uint64_t, void*) {
    const uint64_t cam = c->x[20];
    if (!cam || !mem::is_mapped(cam, 360)) return 0;
    remember_cam(cam);
    fix_camera(cam);
    if (wide().taller_fov && wide().aspect < kAspect169) taller_fields(cam);   // the next projection build is taller at the source
    return 0;
}

int cam_index(uint64_t cam) {
    for (int i = 0; i < 8; ++i)
        if (wide().cams[i].load(std::memory_order_relaxed) == cam) return i;
    return -1;
}

// [pkp] taller_fov, narrower than 16:9 (16:10): the camera itself gets the taller shape before the builder computes P, so every later use of
// its projection agrees (the matrix-only fix_camera path patches P at the render-view hand-over, after the game may already have used the
// 16:9 one: objects then sat at the wrong height, #73). The horizontal reach stays the game's: tan(fov' / 2) = tan(fov / 2) * (16 / 9) /
// aspect, +332 = aspect, so P[0][0] = cot(fov' / 2) / aspect is the 16:9 one and P[1][1] grows. fov_put[] remembers what was written: a
// FOV that differs is a new game value (zoom, cutscene) and is converted again; ours is never converted twice.
void taller_fields(uint64_t cam) {
    Wide& w = wide();
    const int i = cam_index(cam);
    if (i < 0 || (rd<uint32_t>(cam + 344) & 1)) return;   // unknown, or orthographic
    const float a = rd<float>(cam + kCamAspect), f = rd<float>(cam + kCamFov);
    uint32_t fb;
    std::memcpy(&fb, &f, 4);
    if (close_to(a, w.aspect) && fb == w.fov_put[i].load(std::memory_order_relaxed)) return;   // already ours
    if (!close_to(a, kAspect169) && !close_to(a, w.aspect)) return;                             // some other shape: leave it
    if (!(f > 0.05f && f < 3.0f)) return;
    const float nf = static_cast<float>(2.0 * std::atan(std::tan(f * 0.5) * kAspect169 / w.aspect));
    wr<float>(cam + kCamFov, nf);
    wr<float>(cam + kCamAspect, static_cast<float>(w.aspect));
    std::memcpy(&fb, &nf, 4);
    w.fov_put[i].store(fb, std::memory_order_relaxed);
    w.fov_puts.fetch_add(1, std::memory_order_relaxed);
}

int hook_proj_build(::Ctx* c, uint64_t, void*) {
    Wide& w = wide();
    const uint64_t cam = c->x[0];
    if (!cam || !known_cam(cam)) return 0;
    if (w.aspect < kAspect169 && w.taller_fov) {
        taller_fields(cam);
        return 0;
    }
    if (!(w.aspect > kAspect169)) return 0;
    if (close_to(rd<float>(cam + kCamAspect), kAspect169)) {
        wr<float>(cam + kCamAspect, static_cast<float>(w.aspect));
        w.aspect_puts.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

int hook_ui_camera(::Ctx* c, uint64_t, void*) {
    Wide& w = wide();
    const uint64_t sz = c->x[2];
    if (!sz || !w.ui_size || !mem::is_mapped(sz, 8)) return 0;
    if (rd<int32_t>(sz) != kTableW || rd<int32_t>(sz + 4) != kTableH) return 0;
    c->x[2] = w.ui_size;
    w.ui_fixes.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

}  // namespace nxrt::games::pkp::play_detail
