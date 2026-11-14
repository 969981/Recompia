// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim's one ray (pkp_aim_ray.h).
#include "input/pkp_aim_ray.h"
#include "input/pkp_aim_debug.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace nxrt::games::pkp::aim_ray {
namespace {

using guest::rd;
using layout::kPosition;
using layout::kWorld;
constexpr uint64_t kCollisionMeta = 0x355d3c6;   // u16[block id]: bit 0 = solid for the tool rays
constexpr int kBox = 2;                          // cells from the character's column tested in every direction (past it: in front only)
constexpr float kCellSize = 100.f, kOrigin = 102400.f;

// The cast an answer was given for: the frame, the character, its position and the view.
struct Key {
    uint64_t frame = 0, ch = 0;
    float pos[3] = {};
    View view;
};

struct State {
    uint64_t base = 0;
    int extra = 0;
    uint64_t frame = 1;
    Key key;   // frame 0: none yet
    Aim aim;
};
State g;

// The same cast: bit for bit.
bool same(const Key& a, const Key& b) {
    return a.frame == b.frame && a.ch == b.ch && std::memcmp(a.pos, b.pos, sizeof a.pos) == 0 &&
           std::memcmp(a.view.eye, b.view.eye, sizeof a.view.eye) == 0 && std::memcmp(a.view.dir, b.view.dir, sizeof a.view.dir) == 0;
}

// True when cell (x, y, z) holds a block that is solid for the tool rays.
bool solid_cell(int x, int y, int z) {
    if (x < 0 || x > 2047 || z < 0 || z > 2047 || y < 0 || y > 127) return false;
    const uint64_t world = rd<uint64_t>(g.base + kWorld);
    if (!world) return false;
    const uint64_t map = rd<uint64_t>(world + 0x6000030), cells = rd<uint64_t>(world + 0x6000018);
    if (!map || !cells) return false;
    const uint16_t chunk = rd<uint16_t>(map + static_cast<uint64_t>((x >> 5) + (z >> 5) * 64) * 2);
    if (chunk >= 256) return false;
    const uint16_t id = rd<uint16_t>(cells + chunk * 0x60000ull + static_cast<uint64_t>((x & 31) | ((z & 31) << 5) | (y << 10)) * 3) & 0x7ff;
    return id != 0 && (rd<uint16_t>(g.base + kCollisionMeta + 2ull * id) & 1);
}

bool in_world(int x, int y, int z) { return x >= 0 && x <= 2047 && z >= 0 && z <= 2047 && y >= 0 && y <= 127; }

int to_cell(float w) { return static_cast<int>(std::floor((w + kOrigin) / kCellSize)); }

// Walk the cell grid along v from the eye (a grid DDA: each step crosses one face, so the cell before the hit shares a face with it).
// The first solid cell in the box is the ray's cell; prev is the cell the ray was in before it.
void walk(const View& v, uint64_t ch, int box, Aim& a) {
    const float ex = v.eye[0], ey = v.eye[1], ez = v.eye[2];
    const float dx = v.dir[0], dy = v.dir[1], dz = v.dir[2];
    const float px = rd<float>(ch + kPosition), py = rd<float>(ch + kPosition + 4), pz = rd<float>(ch + kPosition + 8);
    if (!(dx * dx + dz * dz > 1e-6f) || !std::isfinite(ey) || !std::isfinite(px) || !std::isfinite(pz)) return;
    const int cx = to_cell(px), cz = to_cell(pz);
    int x = to_cell(ex), y = static_cast<int>(std::floor(ey / kCellSize)), z = to_cell(ez);
    const int sx = dx > 0.f ? 1 : -1, sy = dy > 0.f ? 1 : -1, sz = dz > 0.f ? 1 : -1;
    const float inf = 1e30f;
    const float tdx = std::fabs(dx) > 1e-6f ? kCellSize / std::fabs(dx) : inf, tdy = std::fabs(dy) > 1e-6f ? kCellSize / std::fabs(dy) : inf,
                tdz = std::fabs(dz) > 1e-6f ? kCellSize / std::fabs(dz) : inf;
    float tx = tdx < inf ? ((dx > 0.f ? static_cast<float>(x + 1) * kCellSize - kOrigin - ex : ex - (static_cast<float>(x) * kCellSize - kOrigin)) / std::fabs(dx)) : inf;
    float ty = tdy < inf ? ((dy > 0.f ? static_cast<float>(y + 1) * kCellSize - ey : ey - static_cast<float>(y) * kCellSize) / std::fabs(dy)) : inf;
    float tz = tdz < inf ? ((dz > 0.f ? static_cast<float>(z + 1) * kCellSize - kOrigin - ez : ez - (static_cast<float>(z) * kCellSize - kOrigin)) / std::fabs(dz)) : inf;
    const bool dbg = aim_debug::on();
    if (dbg) {
        const float o[3] = {ex, ey, ez}, d[3] = {dx, dy, dz}, p[3] = {px, py, pz};
        aim_debug::ray_begin(o, d, p, feet_row(ch), box);
    }
    using aim_debug::Step;
    bool near = false;
    for (int n = 0; n < 128; n++) {
        const Cell prev{x, y, z};
        if (tx <= ty && tx <= tz) { x += sx; tx += tdx; }
        else if (ty <= tz) { y += sy; ty += tdy; }
        else { z += sz; tz += tdz; }
        const int h = std::max(std::abs(x - cx), std::abs(z - cz));
        if (h > box) {
            if (near) break;
            if (dbg) aim_debug::ray_step(x, y, z, Step::Far);
            continue;
        }
        near = true;
        if (h > kBox && (static_cast<float>(x) * kCellSize - kOrigin + 50.f - px) * dx + (static_cast<float>(z) * kCellSize - kOrigin + 50.f - pz) * dz <= 0.f) {
            if (dbg) aim_debug::ray_step(x, y, z, Step::Behind);
            continue;
        }
        if (solid_cell(x, y, z)) {
            a.ray = true;
            a.cell = {x, y, z};
            a.h = h;
            a.side = prev;
            return;
        }
        if (dbg) aim_debug::ray_step(x, y, z, Step::Tested);
    }
}

void judge(uint64_t ch, Aim& a) {
    if (!a.ray) return;
    const int d = a.cell.y - feet_row(ch);
    a.reject = a.h > 1 + g.extra                          ? "out of reach (1 + extra cells)"
               : d < -(1 + g.extra) || d > std::max(2, 1 + g.extra) ? "row out of reach"
                                                                     : nullptr;
    a.hit = !a.reject;
    a.adjacent = a.hit && in_world(a.side.x, a.side.y, a.side.z) && !solid_cell(a.side.x, a.side.y, a.side.z);
}

}  // namespace

void configure(uint64_t base, int extra) {
    g = State{};
    g.base = base;
    g.extra = extra;
}

void next_frame() { g.frame++; }

const Aim& cast(const View& v, uint64_t ch) {
    Key k;
    k.frame = g.frame, k.ch = ch, k.view = v;
    for (int i = 0; i < 3; i++) k.pos[i] = rd<float>(ch + kPosition + 4ull * i);
    if (same(k, g.key)) return g.aim;
    g.key = k;
    g.aim = Aim{};
    walk(v, ch, std::max(kBox, 1 + g.extra), g.aim);
    judge(ch, g.aim);
    if (aim_debug::on()) {
        const Aim& a = g.aim;
        aim_debug::ray_result(a.ray, a.cell.x, a.cell.y, a.cell.z, a.h, a.cell.y - feet_row(ch), a.adjacent, a.side.x, a.side.y, a.side.z,
                              a.reject);
    }
    return g.aim;
}

int feet_row(uint64_t ch) { return static_cast<int>(std::floor((rd<float>(ch + kPosition + 4) + 0.2f) / kCellSize)); }

void centre(const Cell& c, float out[3]) {
    out[0] = static_cast<float>(c.x) * kCellSize - kOrigin + 50.f;
    out[1] = static_cast<float>(c.y) * kCellSize + 50.f;
    out[2] = static_cast<float>(c.z) * kCellSize - kOrigin + 50.f;
}

}  // namespace nxrt::games::pkp::aim_ray
