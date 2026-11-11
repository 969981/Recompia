// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim's debug view (pkp_aim_debug.h). The game thread records (camera aim's reports, and once a frame the render view to project
// with); the overlay's presenting thread copies the record under the lock and draws it:
//   (not drawn)   cells the ray walked before it got near the character (not tested; around the crosshair, they would cover the
//                 picture): counted in the panel
//   violet cubes  cells behind the character past the old two-cell box (not tested)
//   blue cubes    cells tested and empty
//   hit cube      green: the last reach check (query first, else a swing's) accepted it; red: rejected (the panel says why); white: no
//                 reach check reported recently
//   yellow cube   the cell the target query selects: its start column (moved or the game's) at the row the row hook chose; a yellow
//                 square at the start cell's floor when the game's own band stayed (the row is then the game's)
//   cyan crosses  a swing shape's position before (dim) and after (bright) the position hook
//   white line    the ray, from a block in front of its origin to where it enters the hit cell (or 15 blocks): it starts at the crosshair
//                 and runs along the view, so it shows as a dot in the middle of the picture; a visible line means the ray and the picture
//                 disagree (another view, a stale frame)
// Cell (x, y, z) spans world x * 100 - 102400 .. + 100, y * 100 .. + 100, z * 100 - 102400 .. + 100 (pkp_aim.cpp solid_cell).
#include "input/pkp_aim_debug.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/pkp_settings.h"
#include "nxrt/overlay_canvas.h"
#include "render/pkp_view_projection.h"

namespace nxrt::games::pkp {
namespace aim_debug {
namespace {

namespace vp = view_projection;

constexpr int kMaxCells = 64;
constexpr float kCell = 100.f, kOrigin = 102400.f;   // world units per cell; the x / z offset of cell 0
constexpr uint64_t kFresh = 2;                        // a report at most this many frames old is this frame's
constexpr uint64_t kStale = 120;                      // older than this: dimmed in the panel
constexpr int kUses = static_cast<int>(Use::Count);
constexpr const char* kUseName[kUses] = {"query", "swing preview", "swing hit", "other"};

struct Cell {
    int16_t x, y, z;
    Step step;
};
struct Ray {
    uint64_t frame = 0;
    bool valid = false, hit = false;
    float origin[3] = {}, dir[3] = {}, character[3] = {};
    int feet = 0, reach = 0;
    Cell cells[kMaxCells];
    int n = 0;
    int hx = 0, hy = 0, hz = 0, hh = 0;
};
struct Row {
    uint64_t frame = 0;
    bool valid = false, hit = false;
    int base = 0, d = 0, band = 0;
    const char* why = "";
    float before = 0, after = 0;
};
struct Pick {
    uint64_t frame = 0;
    bool valid = false, hit = false;
    int x = 0, y = 0, z = 0, h = 0, d = 0;
    const char* reject = nullptr;
};
struct Start {
    uint64_t frame = 0;
    bool valid = false;
    int x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
};
struct Swing {
    uint64_t frame = 0;
    bool valid = false;
    float before[3] = {}, after[3] = {};
    const char* how = "";
};
struct Record {
    uint64_t frame = 0;   // game frames counted while the view is on
    vp::View view;
    Ray ray;
    Row rows[kUses];
    Pick picks[kUses];
    Start start;
    Swing swings[kUses];
};

struct State {
    uint64_t base = 0;
    std::atomic<bool> on{false};
    bool was_on = false;   // the game thread's last read of the key
    std::mutex mu;
    Record rec;
};
State g;

// ---- drawing (the overlay's presenting thread) ----

struct Painter {
    gfx::OverlayCanvas& c;
    const vp::View& v;

    // A world segment, clipped at the eye's near plane (clip space is linear in the world point, so the cut is exact).
    void segment(const float a[3], const float b[3], uint32_t rgba, float thick) const {
        vp::Clip ca = vp::to_clip(v, a), cb = vp::to_clip(v, b);
        if (ca.w < vp::kNear && cb.w < vp::kNear) return;
        auto cut = [](vp::Clip& out, const vp::Clip& in) {
            const float t = (vp::kNear - out.w) / (in.w - out.w);
            out = {out.x + (in.x - out.x) * t, out.y + (in.y - out.y) * t, vp::kNear};
        };
        if (ca.w < vp::kNear) cut(ca, cb);
        else if (cb.w < vp::kNear) cut(cb, ca);
        float x0, y0, x1, y1;
        vp::to_picture(v, ca, c.width(), c.height(), &x0, &y0);
        vp::to_picture(v, cb, c.width(), c.height(), &x1, &y1);
        c.line(x0, y0, x1, y1, rgba, thick);
    }
    // The cell's box, grown by `grow` world units on every side (negative: shrunk, so neighbours' edges do not overlap).
    void cube(int x, int y, int z, uint32_t rgba, float thick, float grow) const {
        const float x0 = static_cast<float>(x) * kCell - kOrigin - grow, y0 = static_cast<float>(y) * kCell - grow,
                    z0 = static_cast<float>(z) * kCell - kOrigin - grow, s = kCell + 2.f * grow;
        float p[8][3];
        for (int i = 0; i < 8; ++i) p[i][0] = x0 + ((i & 1) ? s : 0.f), p[i][1] = y0 + ((i & 2) ? s : 0.f), p[i][2] = z0 + ((i & 4) ? s : 0.f);
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : kEdges) segment(p[e[0]], p[e[1]], rgba, thick);
    }
    // The bottom face only (a cell whose row is not known).
    void floor_square(int x, int y, int z, uint32_t rgba, float thick) const {
        const float x0 = static_cast<float>(x) * kCell - kOrigin, y0 = static_cast<float>(y) * kCell, z0 = static_cast<float>(z) * kCell - kOrigin;
        const float p[4][3] = {{x0, y0, z0}, {x0 + kCell, y0, z0}, {x0 + kCell, y0, z0 + kCell}, {x0, y0, z0 + kCell}};
        for (int i = 0; i < 4; ++i) segment(p[i], p[(i + 1) % 4], rgba, thick);
    }
    void cross(const float p[3], uint32_t rgba, float size) const {
        const vp::Clip k = vp::to_clip(v, p);
        if (k.w < vp::kNear) return;
        float x, y;
        vp::to_picture(v, k, c.width(), c.height(), &x, &y);
        c.line(x - size, y - size, x + size, y + size, rgba, 2.f);
        c.line(x - size, y + size, x + size, y - size, rgba, 2.f);
    }
};

// Where the ray (origin o, unit direction d) enters the cell's box; false when it misses it or the box is behind.
bool ray_entry(const Ray& r, float& t_out) {
    const float lo[3] = {static_cast<float>(r.hx) * kCell - kOrigin, static_cast<float>(r.hy) * kCell, static_cast<float>(r.hz) * kCell - kOrigin};
    float t0 = 0.f, t1 = 1e9f;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(r.dir[a]) < 1e-6f) {
            if (r.origin[a] < lo[a] || r.origin[a] > lo[a] + kCell) return false;
            continue;
        }
        float ta = (lo[a] - r.origin[a]) / r.dir[a], tb = (lo[a] + kCell - r.origin[a]) / r.dir[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta), t1 = std::min(t1, tb);
    }
    if (t0 > t1) return false;
    t_out = t0;
    return true;
}

uint32_t step_color(Step s) { return s == Step::Behind ? 0xc080ff70u : 0x60c0ffa0u; }

// The pick whose colour the hit takes: the query's when fresh, else a swing's.
const Pick* hit_pick(const Record& r) {
    for (Use u : {Use::Query, Use::SwingPreview, Use::SwingHit}) {
        const Pick& p = r.picks[static_cast<int>(u)];
        if (p.valid && r.frame - p.frame <= kFresh) return &p;
    }
    return nullptr;
}

void draw_world(const Painter& p, const Record& r) {
    const Ray& ray = r.ray;
    if (ray.valid && r.frame - ray.frame <= kStale) {
        for (int i = 0; i < ray.n; ++i)
            if (ray.cells[i].step != Step::Far) p.cube(ray.cells[i].x, ray.cells[i].y, ray.cells[i].z, step_color(ray.cells[i].step), 1.f, -4.f);
        float end_t = 1500.f, entry = 0.f;
        if (ray.hit && ray_entry(ray, entry)) end_t = entry;
        const float a[3] = {ray.origin[0] + ray.dir[0] * kCell, ray.origin[1] + ray.dir[1] * kCell, ray.origin[2] + ray.dir[2] * kCell};
        const float b[3] = {ray.origin[0] + ray.dir[0] * end_t, ray.origin[1] + ray.dir[1] * end_t, ray.origin[2] + ray.dir[2] * end_t};
        if (end_t > kCell) p.segment(a, b, 0xffffffc0u, 1.5f);
        if (ray.hit) {
            const Pick* k = hit_pick(r);
            const uint32_t col = !k ? 0xffffffffu : k->reject ? 0xff4040ffu : 0x40ff60ffu;
            p.cube(ray.hx, ray.hy, ray.hz, col, 2.5f, 2.f);
            p.cross(b, col, 6.f);
        }
    }
    // the query's selected cell: its start column at the chosen row
    const Start& s = r.start;
    const Row& q = r.rows[static_cast<int>(Use::Query)];
    if (s.valid && r.frame - s.frame <= kFresh) {
        const bool row_known = q.valid && r.frame - q.frame <= kFresh && std::isfinite(q.after);
        if (row_known && q.band == kUnder && ray.valid) {   // the down extreme: the cell under the character
            p.cube(static_cast<int>(std::floor((ray.character[0] + kOrigin) / kCell)), ray.feet - 1,
                   static_cast<int>(std::floor((ray.character[2] + kOrigin) / kCell)), 0xffe040ffu, 2.f, 6.f);
        } else if (row_known && q.band != kUnder) {
            p.cube(s.nx, s.ny + q.band, s.nz, 0xffe040ffu, 2.f, 6.f);
        } else {
            p.floor_square(s.nx, s.ny, s.nz, 0xffe040ffu, 2.f);
        }
    }
    for (Use u : {Use::SwingPreview, Use::SwingHit}) {
        const Swing& w = r.swings[static_cast<int>(u)];
        if (!w.valid || r.frame - w.frame > kFresh) continue;
        p.cross(w.before, 0x40e0e070u, 5.f);
        p.cross(w.after, 0x40ffffffu, 7.f);
        p.segment(w.before, w.after, 0x40e0e090u, 1.f);
    }
}

void age_suffix(char* buf, size_t n, uint64_t now, uint64_t frame) {
    const size_t k = std::strlen(buf);
    if (now > frame && k + 1 < n) std::snprintf(buf + k, n - k, "   [%llu frames ago]", static_cast<unsigned long long>(now - frame));
}

void draw_panel(gfx::OverlayCanvas& c, const Record& r) {
    constexpr int kLines = 20;
    char lines[kLines][192];
    uint32_t colors[kLines];
    int n = 0;
    auto add = [&](uint32_t col, uint64_t frame, const char* fmt, auto... args) {
        if (n >= kLines) return;
        std::snprintf(lines[n], sizeof lines[n], fmt, args...);
        if (frame) age_suffix(lines[n], sizeof lines[n], r.frame, frame);
        colors[n++] = frame && r.frame - frame > kStale ? 0xa0a0a0b0u : col;
    };
    constexpr uint32_t kText = 0xf0f0f0ffu;
    const vp::View& v = r.view;
    if (!v.valid) add(0xff8080ffu, 0, "%s", "Camera aim debug: no render view yet");
    else if (v.matrix) add(kText, 0, "%s", "Camera aim debug   projection: the game's view-projection matrix");
    else add(0xffd080ffu, 0, "Camera aim debug   projection: look-at, FOV %.1f deg, x scale %.3f, y scale %.3f (matrix check failed)",
             static_cast<double>(v.fov * 57.2957795f), static_cast<double>(v.sx), static_cast<double>(v.sy));
    const Ray& ray = r.ray;
    if (!ray.valid) {
        add(kText, 0, "%s", "no ray reported (camera_aim off, or the follow camera is not the active one)");
    } else {
        add(kText, ray.frame, "ray from %.0f %.0f %.0f  dir %.3f %.3f %.3f  reach %d  feet row %d  character %.0f %.0f %.0f", static_cast<double>(ray.origin[0]),
            static_cast<double>(ray.origin[1]), static_cast<double>(ray.origin[2]), static_cast<double>(ray.dir[0]), static_cast<double>(ray.dir[1]),
            static_cast<double>(ray.dir[2]), ray.reach, ray.feet, static_cast<double>(ray.character[0]), static_cast<double>(ray.character[1]),
            static_cast<double>(ray.character[2]));
        int count[3] = {};
        for (int i = 0; i < ray.n; ++i) ++count[static_cast<int>(ray.cells[i].step)];
        if (ray.hit) add(kText, ray.frame, "hit %d,%d,%d  h %d (cells from the character's column)", ray.hx, ray.hy, ray.hz, ray.hh);
        else add(kText, ray.frame, "%s", "hit none (no solid cell near the character)");
        add(kText, ray.frame, "cells walked: %d before near, %d behind, %d tested empty%s", count[0], count[1], count[2], ray.n >= kMaxCells ? " (list full)" : "");
    }
    for (int u = 0; u < kUses; ++u) {
        const Row& w = r.rows[u];
        if (!w.valid) continue;
        if (!w.hit) add(kText, w.frame, "row  %-13s base %d  no hit: %s", kUseName[u], w.base, w.why);
        else if (std::isfinite(w.after))
            add(kText, w.frame, "row  %-13s base %d  d %+d  -> %s  (pitch %.3f -> %.3f)", kUseName[u], w.base, w.d, w.why, static_cast<double>(w.before),
                static_cast<double>(w.after));
        else add(kText, w.frame, "row  %-13s base %d  d %+d  -> %s  (pitch %.3f kept)", kUseName[u], w.base, w.d, w.why, static_cast<double>(w.before));
    }
    for (int u = 0; u < kUses; ++u) {
        const Pick& k = r.picks[u];
        if (!k.valid) continue;
        if (!k.reject) add(0x80ff90ffu, k.frame, "pick %-13s accepted %d,%d,%d  h %d  d %+d", kUseName[u], k.x, k.y, k.z, k.h, k.d);
        else if (k.hit) add(0xff9090ffu, k.frame, "pick %-13s rejected: %s  (hit %d,%d,%d  h %d  d %+d)", kUseName[u], k.reject, k.x, k.y, k.z, k.h, k.d);
        else add(0xff9090ffu, k.frame, "pick %-13s rejected: %s", kUseName[u], k.reject);
    }
    if (r.start.valid) {
        const Start& s = r.start;
        add(0xffe880ffu, s.frame, "query start cell %d,%d,%d -> %d,%d,%d  (%s)", s.x, s.y, s.z, s.nx, s.ny, s.nz,
            s.x == s.nx && s.y == s.ny && s.z == s.nz ? "kept" : "moved to the hit cell");
    }
    for (int u = 0; u < kUses; ++u) {
        const Swing& w = r.swings[u];
        if (!w.valid) continue;
        add(0x80f0f0ffu, w.frame, "%s position %.0f %.0f %.0f -> %.0f %.0f %.0f  (%s)", kUseName[u], static_cast<double>(w.before[0]),
            static_cast<double>(w.before[1]), static_cast<double>(w.before[2]), static_cast<double>(w.after[0]), static_cast<double>(w.after[1]),
            static_cast<double>(w.after[2]), w.how);
    }
    add(0xb0b0b0ffu, 0, "%s", "violet: behind  blue: tested  hit: green accepted, red rejected  yellow: query cell  cyan: swing position");

    const float lh = c.line_height(), pad = lh * 0.4f;
    float wmax = 0.f;
    for (int i = 0; i < n; ++i) wmax = std::max(wmax, c.text_width(lines[i]));
    c.fill(pad, pad, pad * 3.f + wmax, pad * 3.f + lh * static_cast<float>(n), 0x000000b0u);
    for (int i = 0; i < n; ++i) c.text(pad * 2.f, pad * 2.f + lh * static_cast<float>(i), colors[i], lines[i]);
}

// ---- the feature ----

void install(const FeatureInstall& in) { g.base = in.base; }

void remove() {
    g.on.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g.mu);
    g.rec = Record{};
    g.was_on = false;
}

// The game thread, once a frame: the key (live), and the view to project with.
void game_frame(::Ctx*) {
    const bool on = settings::get_bool(settings::Key::CameraAimDebug);
    g.on.store(on, std::memory_order_relaxed);
    const bool start = on && !g.was_on;
    g.was_on = on;
    if (!on) return;
    const vp::View v = vp::capture(g.base, 0);   // camera aim follows the first player's camera
    std::lock_guard<std::mutex> lk(g.mu);
    if (start) g.rec = Record{};
    g.rec.frame++;
    g.rec.view = v;
}

bool overlay_active() { return g.on.load(std::memory_order_relaxed); }

void overlay_draw(gfx::OverlayCanvas& c) {
    Record r;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        r = g.rec;
    }
    if (r.view.valid) draw_world(Painter{c, r.view}, r);
    draw_panel(c, r);
}

}  // namespace

// ---- the reports (camera aim, game thread) ----

bool on() { return g.on.load(std::memory_order_relaxed); }

void ray_begin(const float origin[3], const float dir[3], const float character[3], int feet_row, int reach) {
    std::lock_guard<std::mutex> lk(g.mu);
    Ray& r = g.rec.ray;
    r = Ray{};
    r.frame = g.rec.frame;
    r.valid = true;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (int i = 0; i < 3; ++i) {
        r.origin[i] = origin[i];
        r.dir[i] = len > 1e-6f ? dir[i] / len : 0.f;
        r.character[i] = character[i];
    }
    r.feet = feet_row;
    r.reach = reach;
}

void ray_step(int x, int y, int z, Step s) {
    std::lock_guard<std::mutex> lk(g.mu);
    Ray& r = g.rec.ray;
    if (r.n < kMaxCells) r.cells[r.n++] = {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), s};
}

void ray_hit(int x, int y, int z, int h) {
    std::lock_guard<std::mutex> lk(g.mu);
    Ray& r = g.rec.ray;
    r.hit = true;
    r.hx = x, r.hy = y, r.hz = z, r.hh = h;
}

void row(Use u, bool hit, int base, int d, int band, const char* why, float pitch_before, float pitch_after) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.rec.rows[static_cast<int>(u)] = {g.rec.frame, true, hit, base, d, band, why, pitch_before, pitch_after};
}

void pick(Use u, bool hit, int x, int y, int z, int h, int d, const char* reject) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.rec.picks[static_cast<int>(u)] = {g.rec.frame, true, hit, x, y, z, h, d, reject};
}

void query_start(int x, int y, int z, int new_x, int new_y, int new_z) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.rec.start = {g.rec.frame, true, x, y, z, new_x, new_y, new_z};
}

void swing_pos(Use u, const float before[3], const float after[3], const char* how) {
    std::lock_guard<std::mutex> lk(g.mu);
    Swing& w = g.rec.swings[static_cast<int>(u)];
    w.frame = g.rec.frame;
    w.valid = true;
    std::memcpy(w.before, before, sizeof w.before);
    std::memcpy(w.after, after, sizeof w.after);
    w.how = how;
}

}  // namespace aim_debug

const Feature kAimDebugFeature = {.name = "aim_debug",
                                  .install = &aim_debug::install,
                                  .remove = &aim_debug::remove,
                                  .game_frame = &aim_debug::game_frame,
                                  .overlay_active = &aim_debug::overlay_active,
                                  .overlay_draw = &aim_debug::overlay_draw};

}  // namespace nxrt::games::pkp
