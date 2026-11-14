// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim: the highlighted cell or area of an ability, tool or held item is aimed where the camera looks, not where the character
// faces. [pkp] camera_aim (on by default; 0 turns it off), [pkp] camera_aim_reach (extra cells, default 0 = the game's own range).
//
// One aim, the game's own consumers. The game already has a free aim: its Joy-Con 2 mouse mode casts the pointer's ray once a frame
// (0x710031935c) into the character's hit cell (+0xaa30), the empty cell beside it (+0xaa36) and the hit cell's centre (+0xaa50), and each
// targeting path has a branch that reads them while that mode is on, then rejoins its normal path. Camera aim casts its own ray once a
// frame instead (pkp_aim_ray.h: from the camera through the crosshair, the first solid cell near the character, in reach) and, for the
// local player, applies at each of those joins what the game's mouse branch writes there, from that one result. So which path breaks and
// which places is the game's decision, not ours:
//   target query 0x71002e19a0 (every ability, tool and held item asks it: Water Gun, Rototiller, Leafage, the placement collector and its
//     validator, the held item's preview): the join 0x71002e1de8 after its mouse branch (0x71002e1dbc..0x71002e1de4). The start cell
//     ([x29 - 0xb8] x, y, [x29 - 0xb4] z) becomes the adjacent cell, the row helper's band kind ([x29 - 0xd8]) and its flags (w21, w22,
//     [sp + 0xa0]) 0 and the query mode ([sp + 0xa4]) 3, as the game does. A held block goes into the cell on the side the player looks at.
//     Not for the nearby-object interaction 0x71002f7340 (the A button's target keeps the game's forward cell; its return 0x71002f73a8).
//   break collector 0x7100588694: the join 0x7100588778 after its mouse branch: the cell ([x29 - 0x20], s16 x, y, z) becomes the hit.
//   swing preview 0x7100721e40 (Rock Smash, Cut and their upgrades): the join 0x71007226bc (right before the shape constructor
//     0x710009db4c): the shape's position ([x29 - 0x60]) becomes the hit cell's centre, as the game's +0xaa50.
//   swing live hit 0x71006d9b40: its position ([x29 - 0x80], tool path only: w22 set, x27 = the character) at 0x71006da610, the same.
//     The game's mouse mode takes the character's stored attack position (+0x35c0) there, set from its preview at the press; camera aim
//     holds the press's view for the action (below), so the live hit is the preview's cell.
// With no target (nothing solid in reach), the game's own facing pick stays, turned to the camera: the heading the query (0x71002e1a14,
// s0, x28 = the character) and the two swing shapes (0x71007220dc, s9, x22; 0x71006d9d70, s8, x19) read from the character is replaced
// by the aim's yaw (the camera's, or the direction to the target when there is one), and the swing positions are turned around the
// character to it, keeping their distance (turn_shape): the character's stored attack position lies in front of where it walks.
//
// When an ability's action really starts (0x710027e5d8 in StartAction, right before it records the action id: w28 = id, x19 = the action
// component) the character snaps to the aim's yaw (its copy, the movement controller's and, in the save-data mode, the save's), so the
// animation plays toward the target. The aim is held for the action (#535): the view it was taken from is kept at that start and every
// hook reads the kept one while the action runs (the action component's id, layout::kActionId, still that ability's); when the action ends
// (its id goes to 0, or another action starts) the live camera aims again.
//
// With the over-the-shoulder camera (pkp_shoulder_camera.cpp) the view is moved off the follow camera by an offset (pkp_view_offset.h): the
// ray starts at the eye plus that offset, parallel to the camera's own, so it still goes through the crosshair.
//
// [pkp] camera_aim_debug draws the ray, its cells and the joins' choices over the picture (pkp_aim_debug.h).
//
// The game's own Joy-Con 2 mouse mode (character + 0xaa81, the Mouse mode key: pkp_mouse_mode.cpp) aims with its pointer: while it is on,
// every hook here leaves the game alone (follow_camera answers 0) and the game's mouse branches run with the pointer's ray.
#include "input/pkp_aim.h"
#include "input/pkp_aim_debug.h"
#include "input/pkp_aim_ray.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_view_offset.h"

#include <cmath>
#include <cstring>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kHeadingRead = 0x2e1a14;      // mid: in the target query after the heading call (s0 = heading, x28 = character)
constexpr uint64_t kSwingPreview = 0x7220dc;     // mid: in the swing preview shape after the heading load (s9 = heading, x22 = character)
constexpr uint64_t kSwingLive = 0x6d9d70;        // mid: in the live hit shape after the heading load (s8 = heading, x19 = character)
constexpr uint64_t kQueryJoin = 0x2e1de8;        // mid: the target query's join after its mouse branch (x28 = character)
constexpr uint64_t kBreakJoin = 0x588778;        // mid: the break collector's join after its mouse branch (x22 = character, cell at x29 - 0x20)
constexpr uint64_t kSwingPreviewJoin = 0x7226bc; // mid: the swing preview's join after its mouse branch (x22 = character, position at x29 - 0x60)
constexpr uint64_t kSwingLivePos = 0x6da610;     // mid: the live hit once its position is chosen (w22 = tool path, x27 = character, position at x29 - 0x80)
constexpr uint64_t kInteractReturn = 0x2f73a8;   // the target query's return into the nearby-object interaction 0x2f7340
constexpr uint64_t kActionStart = 0x27e5d8;      // mid: StartAction, the action really starts (w28 = action id, x19 = action component)
constexpr uint64_t kActionOwner = 0x978;         // action component: the character
using layout::kAction;
using layout::kActionId;
constexpr uint32_t kFirstAbility = 878, kLastAbility = 885;  // the abilities' actions (Leafage, Rototiller, Water Gun, swing tools)
// character heading (layout::kHeading), save heading: both kPlayerMode and kPlayerModeByte 0 = the heading lives in the save data
using layout::kHeading;
using layout::kSaveRoot;
using layout::kSaveHeading;
using layout::kPlayerMode;
using layout::kPlayerModeByte;
constexpr uint64_t kMoveTable = 0x356a778;       // qword: + 0x58 + slot * 8 = the movement component of a local slot
using layout::kController;                       // movement component: the controller; + kControllerYaw its heading (what the model and attacks use)
constexpr uint64_t kControllerYaw = 0x64;
using layout::kPosition;
using layout::kPlayerManager;
constexpr uint64_t kCameraManager = 0x3561948;   // qword: the camera manager
using layout::kManagerPlayer;
using layout::kMouseModeOn;
constexpr uint64_t kFollow = 0x10;               // camera manager + 0x10: the embedded follow camera of slot 0
constexpr uint64_t kActiveTable = 0xc58;         // + signed byte(manager + 0xcc8) * 8: the active camera pointer of slot 0
constexpr uint64_t kActiveIndex = 0xcc8;
constexpr uint64_t kLook = 0x50, kEye = 0x60;    // follow camera: look-at and eye (vec4)
constexpr uint64_t kLogs = 8;                    // target changes logged

using guest::rd;
using guest::wr;
using aim_ray::Aim;
using aim_ray::View;

// The aim of the ability action that started last (hook_action_start), held while that action runs.
struct Held {
    uint64_t ch = 0;   // 0: nothing held
    uint16_t action = 0;
    View view;
};

struct State {
    uint64_t base = 0;
    Held held;
    uint64_t logged = 0;
    uint64_t logged_target = 0;
    aim_ray::Cell last_target{-1, -1, -1};
};
State g;

// The follow camera of the local player's character ch while it is the active one, else 0.
uint64_t follow_camera(uint64_t ch) {
    const uint64_t mgr = rd<uint64_t>(g.base + kPlayerManager);
    if (!mgr || ch != rd<uint64_t>(mgr + kManagerPlayer)) return 0;
    if (rd<uint8_t>(ch + kMouseModeOn)) return 0;   // the game's mouse mode aims with its pointer
    const uint64_t cm = rd<uint64_t>(g.base + kCameraManager);
    if (!cm || !mem::is_mapped(cm, kActiveIndex + 1)) return 0;
    const int8_t slot = rd<int8_t>(cm + kActiveIndex);
    if (slot < 0 || rd<uint64_t>(cm + kActiveTable + static_cast<uint64_t>(slot) * 8) != cm + kFollow) return 0;
    return cm + kFollow;
}

// The follow camera cam's view now. The crosshair is the view's centre: with the over-the-shoulder camera (pkp_view_offset.h) the view is
// moved off the follow camera's eye and look-at by an offset, so the ray starts there and runs parallel (zero offset: the camera's own eye).
View live_view(uint64_t cam) {
    const view_offset::Offset vo = view_offset::get(0);   // the follow camera is the first player's
    const float fx = rd<float>(cam + kEye), fy = rd<float>(cam + kEye + 4), fz = rd<float>(cam + kEye + 8);
    View v;
    v.eye[0] = fx + vo.x, v.eye[1] = fy + vo.y, v.eye[2] = fz + vo.z;
    v.dir[0] = rd<float>(cam + kLook) - fx, v.dir[1] = rd<float>(cam + kLook + 4) - fy, v.dir[2] = rd<float>(cam + kLook + 8) - fz;
    return v;
}

uint16_t action_id(uint64_t ch) {
    const uint64_t act = rd<uint64_t>(ch + kAction);
    return act ? rd<uint16_t>(act + kActionId) : 0;
}

// The view the local player's character ch aims with: the one held for the ability action it runs, else the follow camera's. False when
// ch is not the local player or the follow camera is not the active one.
bool aim_view(uint64_t ch, View& v) {
    const uint64_t cam = follow_camera(ch);
    if (!cam) return false;
    if (g.held.ch) {
        if (g.held.ch == ch && action_id(ch) == g.held.action) {
            v = g.held.view;
            return true;
        }
        g.held = {};   // that action ended
    }
    v = live_view(cam);
    return true;
}

// The aim of the local player's character ch, or null when ch is not the local player or the follow camera is not the active one.
const Aim* aim_of(uint64_t ch) {
    View v;
    if (!aim_view(ch, v)) return nullptr;
    const Aim& a = aim_ray::cast(v, ch);
    if (a.hit && g.logged_target < kLogs &&
        (a.cell.x != g.last_target.x || a.cell.y != g.last_target.y || a.cell.z != g.last_target.z)) {
        g.logged_target++;
        g.last_target = a.cell;
        NXLOG_I(kTag, "camera aim: target %d,%d,%d (%d cells out), adjacent %d,%d,%d%s", a.cell.x, a.cell.y, a.cell.z, a.h, a.side.x, a.side.y,
                a.side.z, a.adjacent ? "" : " (solid or outside the world: none)");
    }
    return &a;
}

// View v's yaw for character ch with aim a: the direction to the target when it is beside the character, else the camera's.
bool aim_yaw(const View& v, const Aim& a, uint64_t ch, float& yaw) {
    const float dx = v.dir[0], dz = v.dir[2];
    if (!(dx * dx + dz * dz > 1e-6f)) return false;
    yaw = std::atan2(dx, dz);
    if (a.hit && a.h > 0) {
        float c[3];
        aim_ray::centre(a.cell, c);
        yaw = std::atan2(c[0] - rd<float>(ch + kPosition), c[2] - rd<float>(ch + kPosition + 8));
    }
    return std::isfinite(yaw);
}

void replace_heading(::Ctx* c, unsigned reg, uint64_t ch, const char* what) {
    View v;
    if (!aim_view(ch, v)) return;
    float yaw;
    if (!aim_yaw(v, aim_ray::cast(v, ch), ch, yaw)) return;
    if (g.logged < kLogs) {
        g.logged++;
        NXLOG_I(kTag, "camera aim: %s heading %.3f -> %.3f", what, static_cast<double>(c->v[reg].f[0]), static_cast<double>(yaw));
    }
    c->v[reg].f[0] = yaw;
}

int hook_heading(::Ctx* c, uint64_t, void*) { replace_heading(c, 0, c->x[28], "query"); return 0; }
int hook_swing_preview(::Ctx* c, uint64_t, void*) { replace_heading(c, 9, c->x[22], "swing preview"); return 0; }
int hook_swing_live(::Ctx* c, uint64_t, void*) { replace_heading(c, 8, c->x[19], "swing hit"); return 0; }

// The target query's join: what its mouse branch (0x71002e1dbc..0x71002e1de4) writes, with the adjacent cell as the pointer's +0xaa36.
int hook_query_join(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[28], fp = c->x[29], sp = c->x[31];
    if (rd<uint64_t>(fp + 8) - g.base == kInteractReturn) return 0;   // the nearby-object interaction keeps the forward cell
    const Aim* a = aim_of(ch);
    if (!a) return 0;
    const int ox = rd<int16_t>(fp - 0xb8), oy = rd<int16_t>(fp - 0xb6), oz = rd<int16_t>(fp - 0xb4);
    const bool move = a->adjacent;
    if (aim_debug::on()) aim_debug::query_start(ox, oy, oz, move ? a->side.x : ox, move ? a->side.y : oy, move ? a->side.z : oz);
    if (!move) return 0;
    wr<int16_t>(fp - 0xb8, static_cast<int16_t>(a->side.x));
    wr<int16_t>(fp - 0xb6, static_cast<int16_t>(a->side.y));
    wr<int16_t>(fp - 0xb4, static_cast<int16_t>(a->side.z));
    wr<uint8_t>(fp - 0xd8, 0);     // the row helper's band kind
    c->x[21] = 0, c->x[22] = 0;    // stored by the join's own stp to [sp + 0x74]
    wr<uint32_t>(sp + 0xa0, 0);
    wr<uint32_t>(sp + 0xa4, 3);    // the query mode
    return 0;
}

// The break collector's join: the cell its mouse branch (0x7100588760..0x7100588774) takes from the pointer's hit (+0xaa30).
int hook_break_join(::Ctx* c, uint64_t, void*) {
    const uint64_t fp = c->x[29];
    const Aim* a = aim_of(c->x[22]);
    if (!a || !a->hit) return 0;
    wr<int16_t>(fp - 0x20, static_cast<int16_t>(a->cell.x));
    wr<int16_t>(fp - 0x1e, static_cast<int16_t>(a->cell.y));
    wr<int16_t>(fp - 0x1c, static_cast<int16_t>(a->cell.z));
    return 0;
}

// No target: the game's position (the character's stored one, +0x35c0, lies in front of where the character faces, so while walking it is
// where the character walks) turned around the character to the aim's yaw, its distance kept.
void turn_shape(uint64_t ch, uint64_t pos, float px, float pz) {
    View v;
    if (!aim_view(ch, v)) return;
    float yaw;
    if (!aim_yaw(v, aim_ray::cast(v, ch), ch, yaw)) return;
    const float cx = rd<float>(ch + kPosition), cz = rd<float>(ch + kPosition + 8);
    const float ox = px - cx, oz = pz - cz, dist = std::sqrt(ox * ox + oz * oz);
    if (!std::isfinite(cx) || !std::isfinite(cz) || !(dist > 1.f) || dist > 400.f) return;   // at the character, or not near it
    wr<float>(pos, cx + std::sin(yaw) * dist);
    wr<float>(pos + 8, cz + std::cos(yaw) * dist);
}

// A swing shape's position (vec4 at pos): the target's centre, as the game's mouse branch takes the pointer's +0xaa50; else turned.
void place_shape(uint64_t ch, uint64_t pos, aim_debug::Use use) {
    const float px = rd<float>(pos), pz = rd<float>(pos + 8);
    if (!std::isfinite(px) || !std::isfinite(pz)) return;
    const Aim* a = aim_of(ch);
    if (!a) return;
    const float before[3] = {px, rd<float>(pos + 4), pz};
    const char* how = "the target's centre";
    if (a->hit) {
        float c[3];
        aim_ray::centre(a->cell, c);
        wr<float>(pos, c[0]);
        wr<float>(pos + 4, c[1]);
        wr<float>(pos + 8, c[2]);
    } else {
        turn_shape(ch, pos, px, pz);
        how = "no target: turned to the camera";
    }
    if (aim_debug::on()) {
        const float after[3] = {rd<float>(pos), rd<float>(pos + 4), rd<float>(pos + 8)};
        aim_debug::swing_pos(use, before, after, how);
    }
}

int hook_swing_preview_join(::Ctx* c, uint64_t, void*) {
    place_shape(c->x[22], c->x[29] - 0x60, aim_debug::Use::SwingPreview);
    return 0;
}

int hook_swing_live_pos(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[27];
    if ((c->x[22] & 0xff) == 0 || !ch) return 0;
    place_shape(ch, c->x[29] - 0x80, aim_debug::Use::SwingHit);
    return 0;
}

// An ability's action really starts: its aim is held until it ends (aim_view) and the character turns to it. The game stores the id at
// kActionId with the next instruction, so the held aim is taken from the live camera here, not through aim_view.
int hook_action_start(::Ctx* c, uint64_t, void*) {
    const uint32_t id = static_cast<uint32_t>(c->x[28]) & 0xffff;
    if (id < kFirstAbility || id > kLastAbility) return 0;
    const uint64_t ch = rd<uint64_t>(c->x[19] + kActionOwner);
    if (!ch) return 0;
    const uint64_t cam = follow_camera(ch);
    if (!cam) return 0;
    g.held = {ch, static_cast<uint16_t>(id), live_view(cam)};
    float yaw;
    if (!aim_yaw(g.held.view, aim_ray::cast(g.held.view, ch), ch, yaw)) return 0;
    std::memcpy(reinterpret_cast<void*>(ch + kHeading), &yaw, sizeof yaw);
    const uint64_t moves = rd<uint64_t>(g.base + kMoveTable);
    const uint64_t move = moves ? rd<uint64_t>(moves + 0x58) : 0;  // ponytail: slot 0, the only player with a follow camera here
    if (move) std::memcpy(reinterpret_cast<void*>(move + kController + kControllerYaw), &yaw, sizeof yaw);
    const uint64_t save = rd<uint64_t>(g.base + kSaveRoot);
    const bool alt = rd<uint32_t>(ch + kPlayerMode) == 0 && rd<uint8_t>(ch + kPlayerModeByte) == 0;
    if (alt && save) std::memcpy(reinterpret_cast<void*>(save + kSaveHeading), &yaw, sizeof yaw);
    return 0;
}

void install(const FeatureInstall& in) {
    g = State{};
    g.base = in.base;
    if (!settings::get_bool(settings::Key::CameraAim)) {
        NXLOG_I(kTag, "camera aim: off");
        return;
    }
    const int extra = static_cast<int>(settings::get_int(settings::Key::CameraAimReach));
    aim_ray::configure(in.base, extra);
    in.hooks.add_mid(in.base + kHeadingRead, &hook_heading);
    in.hooks.add_mid(in.base + kSwingPreview, &hook_swing_preview);
    in.hooks.add_mid(in.base + kSwingLive, &hook_swing_live);
    in.hooks.add_mid(in.base + kQueryJoin, &hook_query_join);
    in.hooks.add_mid(in.base + kBreakJoin, &hook_break_join);
    in.hooks.add_mid(in.base + kSwingPreviewJoin, &hook_swing_preview_join);
    in.hooks.add_mid(in.base + kSwingLivePos, &hook_swing_live_pos);
    in.hooks.add_mid(in.base + kActionStart, &hook_action_start);
    NXLOG_I(kTag, "camera aim: on (one ray a frame; joins main+0x%llx, +0x%llx, +0x%llx, +0x%llx), reach +%d cells",
            static_cast<unsigned long long>(kQueryJoin), static_cast<unsigned long long>(kBreakJoin),
            static_cast<unsigned long long>(kSwingPreviewJoin), static_cast<unsigned long long>(kSwingLivePos), extra);
}

void game_frame(::Ctx*) { aim_ray::next_frame(); }

}  // namespace

const Feature kAimFeature = {.name = "aim", .install = &install, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
