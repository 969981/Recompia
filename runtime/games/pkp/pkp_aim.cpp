// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim: the highlighted cell or area of an ability or tool is aimed where the camera looks, not where the character faces.
// [pkp] camera_aim (off by default).
//
// The game's command target query 0x71002e19a0 (x1 = the character; every ability, tool and item preview and action asks it which
// cells are in front) reads the character's heading through the character's virtual +0x18 (the call returns at 0x71002e1a14 with s0 =
// the heading, radians, atan2(x, z) like player + 0x2404) and picks the forward cells from it. The mid hook at 0x71002e1a14, for the
// local player while the embedded follow camera is the active one, replaces s0 by the follow camera's yaw: atan2 of the horizontal part
// of (look-at + 0x50 - eye + 0x60). Nothing else changes: the character does not turn, the shapes, reach, validity and the breaking and
// placing are the game's.
//
// The swing tools (the command-less actions 881..885: Rock Smash, Cut and their upgrades) do not ask that query. Their area is the
// attack's hit shape placed by the character's transform (virtual +0x38; +0x64 = the heading): the preview in 0x7100721e40 (ldr s9,
// [x0, #0x64] at 0x71007220d8; x22 = the character) and the live hit in 0x71006d9b40 (ldr s8, [x0, #0x64] at 0x71006d9d6c; x19 = the
// character), both feeding the shape constructor 0x71009db4c. Two more mid hooks right after those loads replace that heading by the
// same camera yaw, so the game still builds its own shape, reach and damage, only turned to the camera.
//
// The row (cell y) is chosen by the shared helper 0x71006f77b0 (called by the query, the swing shapes 0x7100721e40 / 0x71006d9b40 and
// the placement collectors): it reads the active camera's pitch (follow camera + 0xb0, negated, wrapped to +-pi/2) and sorts it into
// rows by five tuning angles (tuning1 + 0x414..0x424, degrees: up extreme, +2, +1, 0 lower bound, down extreme), giving the row offset
// -1 / 0 / +1 / +2 from the character's feet row, or the cell under the character for the down extreme. Those angles are set for the
// camera's own pitch, so the row the camera ray crosses in the target column lies about one row below the chosen one. The mid hook at
// 0x71006f7918, right after the wrap (s5 = pitch; s4, s2, s1, s3, s0 = the five angles), computes the row the camera ray (eye + 0x60 ->
// look-at + 0x50) hits: from where the ray enters the forward cell (the cell next to the character in the camera yaw's cardinal
// direction) it is marched 20 units at a time for 6 cells, the first solid block it crosses gives the row, else the row where it
// entered; s5 becomes the middle of the angle band that selects that row offset. A ray that reaches the feet height before the
// forward cell selects the cell under the character (the down extreme).
// The game keeps its column, shapes, reach and validity.
#include "pkp_aim.h"

#include <cmath>
#include <cstring>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kHeadingRead = 0x2e1a14;      // mid: in the target query after the heading call (s0 = heading, x28 = character)
constexpr uint64_t kSwingPreview = 0x7220dc;     // mid: in the swing preview shape after the heading load (s9 = heading, x22 = character)
constexpr uint64_t kSwingLive = 0x6d9d70;        // mid: in the live hit shape after the heading load (s8 = heading, x19 = character)
constexpr uint64_t kPitchRead = 0x6f7918;        // mid: in the row helper after the pitch wrap (s5 = pitch, x22 = character, w21 = extremes allowed)
constexpr uint64_t kWorld = 0x34efc98;           // qword: the world; +0x6000030 = chunk map u16[64*64], +0x6000018 = packed cells
constexpr uint64_t kCollisionMeta = 0x355d3c6;   // u16[block id]: bit 0 = solid for the tool rays
constexpr uint64_t kPosition = 0x20c0;           // character: world position (vec4)
constexpr uint64_t kPlayerManager = 0x3561928;   // qword: the player manager; +0x30 = the local player
constexpr uint64_t kCameraManager = 0x3561948;   // qword: the camera manager
constexpr uint64_t kManagerPlayer = 0x30;
constexpr uint64_t kFollow = 0x10;               // camera manager + 0x10: the embedded follow camera of slot 0
constexpr uint64_t kActiveTable = 0xc58;         // + signed byte(manager + 0xcc8) * 8: the active camera pointer of slot 0
constexpr uint64_t kActiveIndex = 0xcc8;
constexpr uint64_t kLook = 0x50, kEye = 0x60;    // follow camera: look-at and eye (vec4)

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

struct State {
    uint64_t base = 0;
    uint64_t logged = 0;
    uint64_t logged_pitch = 0;
};
State g;

// The follow camera of the local player's character ch while it is the active one, else 0.
uint64_t follow_camera(uint64_t ch) {
    const uint64_t mgr = rd<uint64_t>(g.base + kPlayerManager);
    if (!mgr || ch != rd<uint64_t>(mgr + kManagerPlayer)) return 0;
    const uint64_t cm = rd<uint64_t>(g.base + kCameraManager);
    if (!cm || !mem::is_mapped(cm, kActiveIndex + 1)) return 0;
    const int8_t slot = rd<int8_t>(cm + kActiveIndex);
    if (slot < 0 || rd<uint64_t>(cm + kActiveTable + static_cast<uint64_t>(slot) * 8) != cm + kFollow) return 0;
    return cm + kFollow;
}

// The follow camera's yaw for the local player's character ch, or false when ch is not the local player or the follow camera is not
// the active one.
bool camera_yaw(uint64_t ch, float& yaw) {
    const uint64_t cam = follow_camera(ch);
    if (!cam) return false;
    const float dx = rd<float>(cam + kLook) - rd<float>(cam + kEye), dz = rd<float>(cam + kLook + 8) - rd<float>(cam + kEye + 8);
    if (!(dx * dx + dz * dz > 1e-6f)) return false;
    yaw = std::atan2(dx, dz);
    return std::isfinite(yaw);
}

void replace(::Ctx* c, unsigned reg, uint64_t ch, const char* what) {
    float yaw;
    if (!camera_yaw(ch, yaw)) return;
    if (g.logged < 8) {
        g.logged++;
        NXLOG_I(kTag, "camera aim: %s heading %.3f -> camera yaw %.3f", what, static_cast<double>(c->v[reg].f[0]), static_cast<double>(yaw));
    }
    c->v[reg].f[0] = yaw;
}

int hook_heading(::Ctx* c, uint64_t, void*) { replace(c, 0, c->x[28], "query"); return 0; }
int hook_swing_preview(::Ctx* c, uint64_t, void*) { replace(c, 9, c->x[22], "swing preview"); return 0; }
int hook_swing_live(::Ctx* c, uint64_t, void*) { replace(c, 8, c->x[19], "swing hit"); return 0; }

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

// The row helper sorted the camera pitch (s5) into a row band; replace it by the middle of the band whose row the camera ray hits.
int hook_pitch(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[22];
    const uint64_t cam = follow_camera(ch);
    if (!cam) return 0;
    const float ex = rd<float>(cam + kEye), ey = rd<float>(cam + kEye + 4), ez = rd<float>(cam + kEye + 8);
    const float dx = rd<float>(cam + kLook) - ex, dy = rd<float>(cam + kLook + 4) - ey, dz = rd<float>(cam + kLook + 8) - ez;
    const float px = rd<float>(ch + kPosition), py = rd<float>(ch + kPosition + 4), pz = rd<float>(ch + kPosition + 8);
    if (!(dx * dx + dz * dz > 1e-6f) || !std::isfinite(py)) return 0;
    const int cx = static_cast<int>(std::floor((px + 102400.f) / 100.f)), cz = static_cast<int>(std::floor((pz + 102400.f) / 100.f));
    const int row = static_cast<int>(std::floor(py / 100.f));
    // the forward cell: the cardinal direction of the camera yaw, as the query's own cardinal choice (+Z, +X, -Z, -X)
    const float yaw = std::atan2(dx, dz);
    int fx = cx, fz = cz;
    if (yaw >= 0.7853982f && yaw < 2.3561945f) fx++;
    else if (yaw <= -0.7853982f && yaw > -2.3561945f) fx--;
    else if (std::fabs(yaw) >= 2.3561945f) fz--;
    else fz++;
    // the ray's entry into and exit from that column (x/z slabs)
    const float x0 = static_cast<float>(fx - 1024) * 100.f, z0 = static_cast<float>(fz - 1024) * 100.f;
    float tin = -1e30f, tout = 1e30f;
    if (std::fabs(dx) > 1e-6f) {
        const float a = (x0 - ex) / dx, b = (x0 + 100.f - ex) / dx;
        tin = std::fmax(tin, std::fmin(a, b)); tout = std::fmin(tout, std::fmax(a, b));
    } else if (ex < x0 || ex >= x0 + 100.f) tout = -1.f;
    if (std::fabs(dz) > 1e-6f) {
        const float a = (z0 - ez) / dz, b = (z0 + 100.f - ez) / dz;
        tin = std::fmax(tin, std::fmin(a, b)); tout = std::fmin(tout, std::fmax(a, b));
    } else if (ez < z0 || ez >= z0 + 100.f) tout = -1.f;
    const float t0 = c->v[0].f[0], t1 = c->v[3].f[0], t2 = c->v[1].f[0], t3 = c->v[2].f[0], t4 = c->v[4].f[0];
    float pitch;
    int d = 0;
    const char* how;
    if (tout <= tin || tout <= 0.f) {
        // the ray misses the forward column: the row it crosses at the forward cell's distance
        const float h = std::sqrt(dx * dx + dz * dz), dist = std::sqrt((px - ex) * (px - ex) + (pz - ez) * (pz - ez)) + 50.f;
        d = static_cast<int>(std::floor((ey + dy * dist / h) / 100.f)) - row;
        how = "plane";
    } else if (dy < 0.f && (static_cast<float>(row) * 100.f - ey) / dy < tin) {
        // the ray reaches the feet height before the forward cell: the cell under the character
        d = -2;
        how = "under";
    } else {
        // march the ray from where it enters the forward cell: the first solid cell it crosses within 6 cells gives the row, else the
        // row where it enters
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz), step = 20.f / len;
        float t = std::fmax(tin, 0.f);
        d = static_cast<int>(std::floor((ey + dy * t) / 100.f)) - row;
        how = "entry";
        for (int n = 0; n < 30; n++, t += step) {
            const int x = static_cast<int>(std::floor((ex + dx * t + 102400.f) / 100.f)), y = static_cast<int>(std::floor((ey + dy * t) / 100.f)),
                      z = static_cast<int>(std::floor((ez + dz * t + 102400.f) / 100.f));
            if (solid_cell(x, y, z)) { d = y - row; how = "solid"; break; }
        }
    }
    if (d <= -2) pitch = t0 - 0.01f;                 // the down extreme (when w21 allows it) or the row below
    else if (d == -1) pitch = (t0 + t1) * 0.5f;
    else if (d == 0) pitch = (t1 + t2) * 0.5f;
    else if (d == 1) pitch = (t2 + t3) * 0.5f;
    else pitch = t4 > t3 ? (t3 + t4) * 0.5f : t3;
    if (!std::isfinite(pitch)) return 0;
    if (g.logged_pitch < 8) {
        g.logged_pitch++;
        NXLOG_I(kTag, "camera aim: row pitch %.3f -> %.3f (%s, row %+d, bands %.2f %.2f %.2f %.2f %.2f, extremes %u)",
                static_cast<double>(c->v[5].f[0]), static_cast<double>(pitch), how, d, static_cast<double>(t0), static_cast<double>(t1),
                static_cast<double>(t2), static_cast<double>(t3), static_cast<double>(t4), static_cast<unsigned>(c->x[21] & 0xffffffffu));
    }
    c->v[5].f[0] = pitch;
    return 0;
}

}  // namespace

void aim_install(uint64_t base) {
    if (!config_get_bool("pkp", "camera_aim", false)) return;
    g.base = base;
    hook_add_mid(base + kHeadingRead, &hook_heading);
    hook_add_mid(base + kSwingPreview, &hook_swing_preview);
    hook_add_mid(base + kSwingLive, &hook_swing_live);
    hook_add_mid(base + kPitchRead, &hook_pitch);
    NXLOG_I(kTag, "camera aim: on (hooks main+0x%llx, +0x%llx, +0x%llx, +0x%llx)", static_cast<unsigned long long>(kHeadingRead),
            static_cast<unsigned long long>(kSwingPreview), static_cast<unsigned long long>(kSwingLive),
            static_cast<unsigned long long>(kPitchRead));
}

}  // namespace nxrt::games::pkp
