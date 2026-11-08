// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim: the highlighted cell or area of an ability or tool is aimed where the camera looks, not where the character faces.
// [pkp] camera_aim (off by default).
//
// The game's command target query 0x71002e19a0 (x1 = the character; every ability, tool and item preview and action asks it which
// cells are in front) reads the character's heading through the character's virtual +0x18 (the call returns at 0x71002e1a14 with s0 =
// the heading, radians, atan2(x, z) like player + 0x2404) and picks the forward cells from it. The mid hook at 0x71002e1a14, for the
// local player while the embedded follow camera is the active one, replaces s0 by the follow camera's yaw: atan2 of the horizontal part
// of (look-at + 0x50 - eye + 0x60). Nothing else changes: the character does not turn, the shapes, reach, validity and the breaking and
// placing are the game's. Rock Smash's swing shape (0x7100721e40) has its own facing path and is not touched.
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
};
State g;

int hook_heading(::Ctx* c, uint64_t, void*) {
    const uint64_t ch = c->x[28];
    const uint64_t mgr = rd<uint64_t>(g.base + kPlayerManager);
    if (!mgr || ch != rd<uint64_t>(mgr + kManagerPlayer)) return 0;
    const uint64_t cm = rd<uint64_t>(g.base + kCameraManager);
    if (!cm || !mem::is_mapped(cm, kActiveIndex + 1)) return 0;
    const int8_t slot = rd<int8_t>(cm + kActiveIndex);
    if (slot < 0 || rd<uint64_t>(cm + kActiveTable + static_cast<uint64_t>(slot) * 8) != cm + kFollow) return 0;
    const uint64_t cam = cm + kFollow;
    const float dx = rd<float>(cam + kLook) - rd<float>(cam + kEye), dz = rd<float>(cam + kLook + 8) - rd<float>(cam + kEye + 8);
    if (!(dx * dx + dz * dz > 1e-6f)) return 0;
    const float yaw = std::atan2(dx, dz);
    if (!std::isfinite(yaw)) return 0;
    if (g.logged < 4) {
        g.logged++;
        NXLOG_I(kTag, "camera aim: heading %.3f -> camera yaw %.3f", static_cast<double>(c->v[0].f[0]), static_cast<double>(yaw));
    }
    c->v[0].f[0] = yaw;
    return 0;
}

}  // namespace

void aim_install(uint64_t base) {
    if (!config_get_bool("pkp", "camera_aim", false)) return;
    g.base = base;
    hook_add_mid(base + kHeadingRead, &hook_heading);
    NXLOG_I(kTag, "camera aim: on (hook main+0x%llx)", static_cast<unsigned long long>(kHeadingRead));
}

}  // namespace nxrt::games::pkp
