// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: a player's render view and the projection to the picture (pkp_view_projection.h).
#include "render/pkp_view_projection.h"

#include <cmath>
#include <cstring>

#include "core/pkp_guest.h"

namespace nxrt::games::pkp::view_projection {
namespace {

constexpr uint64_t kViewRoot = 0x3483e18;     // GOT: -> the qword that holds the render views' root (0x7100088ad4..88ae8)
constexpr uint64_t kViewFirst = 0x49b0, kViewStride = 0x260;
constexpr uint64_t kEye = 0x10, kLook = 0x20, kUp = 0x30, kProj = 0xc0, kViewProj = 0x100, kFov = 0x148, kAspect = 0x14c;
constexpr uint64_t kBlockBytes = 0x150;

using guest::rd;

void read3(uint64_t a, float out[3]) { std::memcpy(out, reinterpret_cast<const void*>(a), 3 * sizeof(float)); }
float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
bool normalize(float v[3]) {
    const float len = std::sqrt(dot(v, v));
    if (!(len > 1e-6f) || !std::isfinite(len)) return false;
    v[0] /= len, v[1] /= len, v[2] /= len;
    return true;
}
bool sane_scale(float s) { return std::isfinite(s) && std::fabs(s) > 0.05f && std::fabs(s) < 50.f; }

Clip matrix_clip(const float m[16], const float p[3]) {
    return {p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12], p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
            p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15]};
}

Clip lookat_clip(const View& v, const float p[3]) {
    float f[3] = {v.look[0] - v.eye[0], v.look[1] - v.eye[1], v.look[2] - v.eye[2]}, r[3], u[3];
    if (!normalize(f)) return {};
    cross(f, v.up, r);
    if (!normalize(r)) return {};
    cross(r, f, u);
    const float d[3] = {p[0] - v.eye[0], p[1] - v.eye[1], p[2] - v.eye[2]};
    return {dot(d, r) * v.sx, dot(d, u) * v.sy, dot(d, f)};
}

}  // namespace

View capture(uint64_t base, int player) {
    View v;
    const uint64_t got = rd<uint64_t>(base + kViewRoot);
    const uint64_t root = guest::ok(got, 8) ? rd<uint64_t>(got) : 0;
    if (!root || player < 0 || player > 3) return v;
    const uint64_t block = root + static_cast<uint64_t>(player) * kViewStride + kViewFirst;
    if (!guest::ok(block, kBlockBytes)) return v;
    read3(block + kEye, v.eye);
    read3(block + kLook, v.look);
    read3(block + kUp, v.up);
    std::memcpy(v.vp, reinterpret_cast<const void*>(block + kViewProj), sizeof v.vp);
    v.fov = rd<float>(block + kFov);
    v.aspect = rd<float>(block + kAspect);
    const float p00 = rd<float>(block + kProj), p11 = rd<float>(block + kProj + 20);
    for (float f : v.eye)
        if (!std::isfinite(f)) return v;
    for (float f : v.look)
        if (!std::isfinite(f)) return v;
    float fwd[3] = {v.look[0] - v.eye[0], v.look[1] - v.eye[1], v.look[2] - v.eye[2]};
    const float dist = std::sqrt(dot(fwd, fwd));
    if (!normalize(fwd) || !normalize(v.up)) return v;

    // the look-at way's scales: the projection's diagonal when sane, else the FOV field (and the aspect field, else 16:9)
    const float fov_scale = v.fov > 0.1f && v.fov < 3.f ? 1.f / std::tan(v.fov * 0.5f) : 1.f / std::tan(0.3927f);
    v.sy = sane_scale(p11) ? std::fabs(p11) : fov_scale;
    v.sx = sane_scale(p00) ? std::fabs(p00) : v.sy / (v.aspect > 0.3f && v.aspect < 5.f ? v.aspect : 16.f / 9.f);

    // the matrix way holds when the look-at lands in the middle of the picture at its own depth, and up moves it on screen
    bool vp_ok = true;
    for (float f : v.vp) vp_ok = vp_ok && std::isfinite(f);
    if (vp_ok) {
        const Clip c = matrix_clip(v.vp, v.look);
        const float above[3] = {v.look[0] + v.up[0] * dist * 0.1f, v.look[1] + v.up[1] * dist * 0.1f, v.look[2] + v.up[2] * dist * 0.1f};
        const Clip a = matrix_clip(v.vp, above);
        if (c.w > kNear && std::fabs(c.w - dist) < 0.05f * dist + 1.f && std::fabs(c.x / c.w) < 0.02f && std::fabs(c.y / c.w) < 0.02f && a.w > kNear) {
            const float dy = a.y / a.w - c.y / c.w;
            if (std::fabs(dy) > 1e-4f) {
                v.matrix = true;
                v.y_sign = dy > 0.f ? 1.f : -1.f;
            }
        }
    }
    v.valid = true;
    return v;
}

Clip to_clip(const View& v, const float p[3]) { return v.matrix ? matrix_clip(v.vp, p) : lookat_clip(v, p); }

void to_picture(const View& v, const Clip& c, float w, float h, float* px, float* py) {
    *px = (c.x / c.w * 0.5f + 0.5f) * w;
    *py = (0.5f - v.y_sign * c.y / c.w * 0.5f) * h;
}

}  // namespace nxrt::games::pkp::view_projection
