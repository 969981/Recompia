// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim: the highlighted cell or area of an ability or tool is aimed where the camera looks, not where the character faces.
// [pkp] camera_aim (on by default; 0 turns it off).
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
// -1 / 0 / +1 / +2 from the character's feet row, or the cell under the character for the down extreme. The mid hook at 0x71006f7918,
// right after the wrap (s5 = pitch; s4, s2, s1, s3, s0 = the five angles), walks the cell grid along the camera ray (eye + 0x60 ->
// look-at + 0x50); the first solid cell within two cells of the character gives the row, and s5 becomes the middle of the angle band
// that selects it. With no solid cell near, or a row out of the bands' range, the game's own band stays.
// The game keeps its column, shapes, reach and validity.
//
// A floor cell the ray hits beside the character also gives the heading (the direction to that cell), so looking down picks that cell.
// When an ability's action really starts (0x710027e5d8 in StartAction, right before it records the action id: w28 = id, x19 = the
// action component) the character snaps to the same heading (its copy, the movement controller's and, in the save-data mode, the save's), so the animation plays toward the target. It does not turn otherwise.
#include "pkp_aim.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
constexpr uint64_t kActionStart = 0x27e5d8;      // mid: StartAction, the action really starts (w28 = action id, x19 = action component)
constexpr uint64_t kActionOwner = 0x978;         // action component: the character
constexpr uint32_t kFirstAbility = 878, kLastAbility = 885;  // the abilities' actions (Leafage, Rototiller, Water Gun, swing tools)
constexpr uint64_t kHeading = 0x2404;            // character: heading (radians), a copy of the movement controller's
constexpr uint64_t kSaveRoot = 0x3561930, kSaveHeading = 0x741a10, kPlayerMode = 0x5a34, kPlayerModeByte = 0x5a39;  // both 0: the heading lives in the save data
constexpr uint64_t kMoveTable = 0x356a778;       // qword: + 0x58 + slot * 8 = the movement component of a local slot
constexpr uint64_t kController = 0xbc0, kControllerYaw = 0x64;  // movement component: the controller and its heading (what the model and attacks use)
constexpr uint64_t kPosition = 0x20c0;           // character: world position (vec4)
constexpr uint64_t kPlayerManager = 0x3561928;   // qword: the player manager; +0x30 = the local player
constexpr uint64_t kCameraManager = 0x3561948;   // qword: the camera manager
constexpr uint64_t kManagerPlayer = 0x30;
constexpr uint64_t kFollow = 0x10;               // camera manager + 0x10: the embedded follow camera of slot 0
constexpr uint64_t kActiveTable = 0xc58;         // + signed byte(manager + 0xcc8) * 8: the active camera pointer of slot 0
constexpr uint64_t kActiveIndex = 0xcc8;
constexpr uint64_t kLook = 0x50, kEye = 0x60;    // follow camera: look-at and eye (vec4)
constexpr uint64_t kSwingPreviewReturn = 0x7223cc, kSwingLiveReturn = 0x6da458;  // the row helper's return into the swing shapes
constexpr uint64_t kStepRows = 0x5a84, kStepFlag = 0x5a8c;  // character: step count ahead (int) and its flag; the swing shapes raise by it
constexpr int kReach = 2;                          // cells from the character's column within which a hit is accepted

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

int feet_row(uint64_t ch) { return static_cast<int>(std::floor((rd<float>(ch + kPosition + 4) + 0.2f) / 100.f)); }

struct RayHit { int x, y, z, h; };  // h: cells from the character's column

// Walk the cell grid along the camera ray from the eye; the first solid cell within kReach of the character's column is the hit. Cells
// before the ray gets near the character are skipped.
bool camera_ray(uint64_t cam, uint64_t ch, RayHit& r) {
    const float ex = rd<float>(cam + kEye), ey = rd<float>(cam + kEye + 4), ez = rd<float>(cam + kEye + 8);
    const float dx = rd<float>(cam + kLook) - ex, dy = rd<float>(cam + kLook + 4) - ey, dz = rd<float>(cam + kLook + 8) - ez;
    const float px = rd<float>(ch + kPosition), pz = rd<float>(ch + kPosition + 8);
    if (!(dx * dx + dz * dz > 1e-6f) || !std::isfinite(ey) || !std::isfinite(px) || !std::isfinite(pz)) return false;
    const int cx = static_cast<int>(std::floor((px + 102400.f) / 100.f)), cz = static_cast<int>(std::floor((pz + 102400.f) / 100.f));
    int x = static_cast<int>(std::floor((ex + 102400.f) / 100.f)), y = static_cast<int>(std::floor(ey / 100.f)),
        z = static_cast<int>(std::floor((ez + 102400.f) / 100.f));
    const int sx = dx > 0.f ? 1 : -1, sy = dy > 0.f ? 1 : -1, sz = dz > 0.f ? 1 : -1;
    const float inf = 1e30f;
    const float tdx = std::fabs(dx) > 1e-6f ? 100.f / std::fabs(dx) : inf, tdy = std::fabs(dy) > 1e-6f ? 100.f / std::fabs(dy) : inf,
                tdz = std::fabs(dz) > 1e-6f ? 100.f / std::fabs(dz) : inf;
    float tx = tdx < inf ? ((dx > 0.f ? static_cast<float>(x + 1) * 100.f - 102400.f - ex : ex - (static_cast<float>(x) * 100.f - 102400.f)) / std::fabs(dx)) : inf;
    float ty = tdy < inf ? ((dy > 0.f ? static_cast<float>(y + 1) * 100.f - ey : ey - static_cast<float>(y) * 100.f) / std::fabs(dy)) : inf;
    float tz = tdz < inf ? ((dz > 0.f ? static_cast<float>(z + 1) * 100.f - 102400.f - ez : ez - (static_cast<float>(z) * 100.f - 102400.f)) / std::fabs(dz)) : inf;
    bool near = false;
    for (int n = 0; n < 128; n++) {
        if (tx <= ty && tx <= tz) { x += sx; tx += tdx; }
        else if (ty <= tz) { y += sy; ty += tdy; }
        else { z += sz; tz += tdz; }
        const int h = std::max(std::abs(x - cx), std::abs(z - cz));
        if (h > kReach) { if (near) break; continue; }
        near = true;
        if (solid_cell(x, y, z)) { r = {x, y, z, h}; return true; }
    }
    return false;
}

// The follow camera's yaw for the local player's character ch, or false when ch is not the local player or the follow camera is not
// the active one.
bool camera_yaw(uint64_t ch, float& yaw) {
    const uint64_t cam = follow_camera(ch);
    if (!cam) return false;
    const float dx = rd<float>(cam + kLook) - rd<float>(cam + kEye), dz = rd<float>(cam + kLook + 8) - rd<float>(cam + kEye + 8);
    if (!(dx * dx + dz * dz > 1e-6f)) return false;
    yaw = std::atan2(dx, dz);
    RayHit r;
    if (camera_ray(cam, ch, r) && r.h > 0 && r.y < feet_row(ch)) {
        yaw = std::atan2(static_cast<float>(r.x) * 100.f - 102350.f - rd<float>(ch + kPosition),
                         static_cast<float>(r.z) * 100.f - 102350.f - rd<float>(ch + kPosition + 8));
    }
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

// The row helper sorted the camera pitch (s5) into a row band; replace it by the middle of the band whose row holds the cell the
// camera ray hits.
int hook_pitch(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[22];
    const uint64_t cam = follow_camera(ch);
    if (!cam) return 0;
    const int row = feet_row(ch);
    // the row the game applies the band offset to: the feet row; the swing shapes add the step count when the step flag is set
    const uint64_t ret = rd<uint64_t>(c->x[29] + 8) - g.base;
    const bool swing = ret == kSwingPreviewReturn || ret == kSwingLiveReturn;
    const int steps = swing && rd<uint8_t>(ch + kStepFlag) ? rd<int32_t>(ch + kStepRows) : 0;
    const int base = row + (steps >= -8 && steps <= 8 ? steps : 0);
    RayHit r;
    if (!camera_ray(cam, ch, r)) return 0;           // nothing near: the game's own band
    const int d = r.y - base;
    // A swing's shape spans the band's row and the one above, and the game keeps only the upper one when it holds a block (0x7100409160):
    // ask for the row below the hit. The band of the lowest row keeps the floor in front when the row above it is empty.
    const int band = swing ? std::max(d - 1, -1) : d;
    const float t0 = c->v[0].f[0], t1 = c->v[3].f[0], t2 = c->v[1].f[0], t3 = c->v[2].f[0], t4 = c->v[4].f[0];
    float pitch;
    if (r.h == 0 && r.y != row - 1) return 0;        // the character's own column above the floor: leave it alone
    if (r.h == 0) pitch = t0 - 0.01f;                // the down extreme: the cell under the character
    else if (band == -1) pitch = (t0 + t1) * 0.5f;
    else if (band == 0) pitch = (t1 + t2) * 0.5f;
    else if (band == 1) pitch = (t2 + t3) * 0.5f;
    else if (band == 2) pitch = t4 > t3 ? (t3 + t4) * 0.5f : t3;
    else return 0;                                   // outside the band range: the game's own band
    if (!std::isfinite(pitch)) return 0;
    if (g.logged_pitch < 8) {
        g.logged_pitch++;
        NXLOG_I(kTag, "camera aim: row pitch %.3f -> %.3f (row %+d of base %d, hit %d,%d,%d, bands %.2f %.2f %.2f %.2f %.2f, extremes %u)",
                static_cast<double>(c->v[5].f[0]), static_cast<double>(pitch), d, base, r.x, r.y, r.z, static_cast<double>(t0), static_cast<double>(t1),
                static_cast<double>(t2), static_cast<double>(t3), static_cast<double>(t4), static_cast<unsigned>(c->x[21] & 0xffffffffu));
    }
    c->v[5].f[0] = pitch;
    return 0;
}

// An ability's action really starts: the character turns to the aim.
int hook_action_start(::Ctx* c, uint64_t, void*) {
    const uint32_t id = static_cast<uint32_t>(c->x[28]) & 0xffff;
    if (id < kFirstAbility || id > kLastAbility) return 0;
    const uint64_t ch = rd<uint64_t>(c->x[19] + kActionOwner);
    float yaw;
    if (!camera_yaw(ch, yaw)) return 0;
    std::memcpy(reinterpret_cast<void*>(ch + kHeading), &yaw, sizeof yaw);
    const uint64_t move = rd<uint64_t>(rd<uint64_t>(g.base + kMoveTable) + 0x58);  // ponytail: slot 0, the only player with a follow camera here
    if (move) std::memcpy(reinterpret_cast<void*>(move + kController + kControllerYaw), &yaw, sizeof yaw);
    const uint64_t save = rd<uint64_t>(g.base + kSaveRoot);
    const bool alt = rd<uint32_t>(ch + kPlayerMode) == 0 && rd<uint8_t>(ch + kPlayerModeByte) == 0;
    if (alt && save) std::memcpy(reinterpret_cast<void*>(save + kSaveHeading), &yaw, sizeof yaw);
    return 0;
}

}  // namespace

void aim_install(uint64_t base) {
    if (!config_get_bool("pkp", "camera_aim", true)) return;
    g.base = base;
    hook_add_mid(base + kHeadingRead, &hook_heading);
    hook_add_mid(base + kSwingPreview, &hook_swing_preview);
    hook_add_mid(base + kSwingLive, &hook_swing_live);
    hook_add_mid(base + kPitchRead, &hook_pitch);
    hook_add_mid(base + kActionStart, &hook_action_start);
    NXLOG_I(kTag, "camera aim: on (hooks main+0x%llx, +0x%llx, +0x%llx, +0x%llx, +0x%llx)", static_cast<unsigned long long>(kHeadingRead),
            static_cast<unsigned long long>(kSwingPreview), static_cast<unsigned long long>(kSwingLive),
            static_cast<unsigned long long>(kPitchRead), static_cast<unsigned long long>(kActionStart));
}

}  // namespace nxrt::games::pkp
