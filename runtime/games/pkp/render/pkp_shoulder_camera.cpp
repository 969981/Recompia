// SPDX-License-Identifier: GPL-3.0-or-later
// Over-the-shoulder camera: the follow camera's view moved sideways and up by a distance the player picks, so the character no longer sits
// in the middle of the screen. [pkp] camera_shoulder_side and camera_shoulder_height (blocks, 100 world units each; positive = right / up).
// There is no on/off switch (repo owner's decision): 0 and 0, the defaults, is the game's own camera, and then the hook returns before it
// touches anything.
//
// The follow camera's per-frame update 0x7100088aac writes the render view of its player: the view block
// [[main+0x3561758] + player * 0x260 + 0x49b0] (eye vec4 at +0x10, look-at vec4 at +0x20, up vec4 at +0x30; the player is the signed byte at
// camera + 8). Both of its branches (the normal copy of the camera's eye and look-at, and the strafe-style branch 0x27abc8 -> 0x8a6b4, which
// writes its own view) join at 0x7100088e04, right before `bl 0x10f9270` (the view matrix update from the block). The mid hook there
// (x19 = the follow camera, x20 = the view block) adds the offset to the view's eye and look-at, up unchanged, so the picture moves and
// does not turn. Only the render view changes: the follow camera's own eye and look-at (camera aim, the game's camera logic) keep the
// unshifted values, and camera aim adds the published offset (pkp_view_offset.h) to the ray's origin so the crosshair still hits its cell.
//
// The game does not rewrite the block every frame (it skips the copy when the follow look-at equals its eye), so the offset is never added
// to what is in the block blindly. Per player slot the hook keeps the base eye and look-at it started from and what it wrote: when the block
// still holds exactly what was written (bitwise), the game did not rewrite it and the base is the stored one; otherwise the base is the
// block as it is now.
//
// Walls: a camera moved sideways can end up inside a block. The game's own sphere test 0x710007e820 (float[8] sphere: x, y, z at [0..2],
// radius at [4]; flags; true = it overlaps solid geometry), which the follow placement 0x78440 calls with flags 0x1180 and the radius
// (int at +0x698 of [[main+0x36ca678] + 0x40]), picks the largest share of the offset (1, 3/4, 1/2, 1/4, 0) whose sphere at base eye + share *
// offset is clear. The share snaps down at once and eases up (0 to 1 in about a quarter of a second, by the monotonic clock, whatever the frame rate).
// The test needs the world (the placement returns early without it, so does this).
#include "render/pkp_shoulder_camera.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_view_offset.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kViewWrite = 0x88e04;         // mid: both branches of the follow camera's view write joined (x19 = camera, x20 = view block)
constexpr uint64_t kCameraPlayer = 0x8;          // follow camera: the player index (signed byte)
constexpr uint64_t kViewEye = 0x10;              // view block: eye vec4; the look-at vec4 follows at +0x20, up at +0x30
constexpr uint64_t kViewBytes = 0x40;
constexpr uint64_t kSphereTest = 0x7e820;        // (float* sphere, u32 flags) -> bool: the sphere overlaps solid block geometry
constexpr uint32_t kSphereFlags = 0x1180;        // the follow placement's flags
constexpr uint64_t kTuning = 0x36ca678;          // qword: the tuning root; [[root] + 0x40] + kCameraRadius = the camera's sphere radius (int)
constexpr uint64_t kCameraRadius = 0x698;
constexpr float kFallbackRadius = 20.f;          // world units, when the tuning block is not mapped
constexpr float kBlock = 100.f;                  // world units in a block
constexpr double kEaseSeconds = 0.25;            // a share from 0 to 1
constexpr double kMaxStep = 0.1;                 // a frame gap past this (a pause, a load) is eased as this
constexpr float kShares[] = {1.f, 0.75f, 0.5f, 0.25f};   // tried in this order; none clear = 0
// Which way "right" is: right = (-f.z, 0, f.x) of the horizontal forward f, which is f x up in a right-handed Y-up world (what the game's
// view matrix is assumed to be, no boot confirmed it). One place: set to -1 when side +1 turns out to move the view to the left.
constexpr float kRightSign = 1.f;

struct Slot {
    bool valid = false;           // base and written hold a frame the hook wrote
    float base[8] = {};           // the view's eye (vec4) and look-at (vec4) the game wrote
    float written[8] = {};        // what the hook wrote over them
    float right[3] = {1, 0, 0};   // the last horizontal right vector (kept when the camera looks straight up or down)
    float share = 0.f;            // the part of the offset in force (wall limited, eased)
    uint64_t last_ns = 0;
};

struct State {
    uint64_t base = 0;
    std::atomic<float> side{0.f}, height{0.f};   // world units, set each game frame from the settings
    Slot slots[view_offset::kPlayers];
    std::atomic<uint64_t> shifted{0}, limited{0};
    bool logged = false;
};
State g;

using guest::rd;
using guest::wr;

// True when the camera's sphere at (x, y, z) overlaps solid geometry, by the game's own test.
bool blocked(::Ctx* c, float x, float y, float z, float w) {
    float radius = kFallbackRadius;
    const uint64_t root = rd<uint64_t>(g.base + kTuning);
    const uint64_t block = root && guest::ok(root, 0x48) ? rd<uint64_t>(root + 0x40) : 0;
    if (block && guest::ok(block + kCameraRadius, 4)) radius = static_cast<float>(rd<int32_t>(block + kCameraRadius));
    // the sphere goes below the guest stack pointer, the call's stack under it (the test clears ~0x3e2 bytes and uses ~0x4a0 of stack)
    const uint64_t sphere = (c->x[31] - 0x200) & ~uint64_t{0xf};
    const float v[8] = {x, y, z, w, radius, 0, 0, 0};
    std::memcpy(reinterpret_cast<void*>(sphere), v, sizeof v);
    return (guest::call(c, g.base + kSphereTest, sphere, kSphereFlags, 0, 0, 0, sphere - 0x10) & 0xff) != 0;
}

// The offset (world units) for the horizontal forward (fx, fz) and the settings; updates the slot's right vector.
void offset_vector(Slot& s, float fx, float fz, float side, float height, float out[3]) {
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len > 1e-3f) {
        s.right[0] = kRightSign * -fz / len;
        s.right[1] = 0.f;
        s.right[2] = kRightSign * fx / len;
    }
    out[0] = s.right[0] * side;
    out[1] = height;
    out[2] = s.right[2] * side;
}

// The game's own camera again: when the block still holds what was written, put the base back (the game may not rewrite it this frame).
void release(Slot& s, uint64_t view) {
    if (!s.valid) return;
    if (std::memcmp(reinterpret_cast<const void*>(view + kViewEye), s.written, sizeof s.written) == 0)
        std::memcpy(reinterpret_cast<void*>(view + kViewEye), s.base, sizeof s.base);
    s = Slot{};
}

int hook_view(::Ctx* c, uint64_t, void*) {
    const int player = rd<int8_t>(c->x[19] + kCameraPlayer);
    if (player < 0 || player >= view_offset::kPlayers) return 0;
    const uint64_t view = c->x[20];
    Slot& s = g.slots[player];
    const float side = g.side.load(std::memory_order_relaxed), height = g.height.load(std::memory_order_relaxed);
    if (side == 0.f && height == 0.f) {
        if (s.valid && guest::ok(view, kViewBytes)) release(s, view);
        view_offset::set(player, 0, 0, 0);
        return 0;
    }
    if (!guest::ok(view, kViewBytes)) return 0;

    float cur[8];
    std::memcpy(cur, reinterpret_cast<const void*>(view + kViewEye), sizeof cur);
    if (!s.valid || std::memcmp(cur, s.written, sizeof cur) != 0) std::memcpy(s.base, cur, sizeof cur);   // the game wrote a view
    for (float f : s.base)
        if (!std::isfinite(f)) {
            s = Slot{};
            return 0;
        }

    float off[3];
    offset_vector(s, s.base[4] - s.base[0], s.base[6] - s.base[2], side, height, off);

    // the wall limit: the largest share whose sphere is clear (the placement needs the world; without it there is nothing to test against)
    float target = 1.f;
    if (rd<uint64_t>(g.base + layout::kWorld)) {
        target = 0.f;
        for (float share : kShares)
            if (!blocked(c, s.base[0] + off[0] * share, s.base[1] + off[1] * share, s.base[2] + off[2] * share, s.base[3])) {
                target = share;
                break;
            }
    }
    const uint64_t now = qpc::now_ns();
    const double dt = s.last_ns ? std::min(static_cast<double>(now - s.last_ns) * 1e-9, kMaxStep) : 0.0;
    s.last_ns = now;
    s.share = target < s.share ? target : std::min(target, s.share + static_cast<float>(dt / kEaseSeconds));

    float out[8];
    std::memcpy(out, s.base, sizeof out);
    const float ox = off[0] * s.share, oy = off[1] * s.share, oz = off[2] * s.share;
    out[0] += ox, out[1] += oy, out[2] += oz;   // eye
    out[4] += ox, out[5] += oy, out[6] += oz;   // look-at
    std::memcpy(reinterpret_cast<void*>(view + kViewEye), out, sizeof out);
    std::memcpy(s.written, out, sizeof out);
    s.valid = true;
    view_offset::set(player, ox, oy, oz);

    g.shifted.fetch_add(1, std::memory_order_relaxed);
    if (target < 1.f) g.limited.fetch_add(1, std::memory_order_relaxed);
    if (!g.logged) {
        g.logged = true;
        NXLOG_I(kTag, "shoulder camera: first offset for player %d: side %.2f, height %.2f blocks -> (%.1f, %.1f, %.1f), share %.2f (wall target %.2f)",
                player, static_cast<double>(side / kBlock), static_cast<double>(height / kBlock), static_cast<double>(ox), static_cast<double>(oy),
                static_cast<double>(oz), static_cast<double>(s.share), static_cast<double>(target));
    }
    return 0;
}

void install(const FeatureInstall& in) {
    g.base = in.base;
    in.hooks.add_mid(in.base + kViewWrite, &hook_view);
    NXLOG_I(kTag, "shoulder camera: hook main+0x%llx (0 / 0 = the game's own camera)", static_cast<unsigned long long>(kViewWrite));
}

// The settings, read each game frame (they change while the game runs).
void game_frame(::Ctx*) {
    g.side.store(static_cast<float>(settings::get_double(settings::Key::CameraShoulderSide)) * kBlock, std::memory_order_relaxed);
    g.height.store(static_cast<float>(settings::get_double(settings::Key::CameraShoulderHeight)) * kBlock, std::memory_order_relaxed);
}

void remove() {
    for (int p = 0; p < view_offset::kPlayers; ++p) view_offset::set(p, 0, 0, 0);
    for (Slot& s : g.slots) s = Slot{};
    g.side.store(0.f), g.height.store(0.f);
    g.shifted.store(0), g.limited.store(0);
    g.logged = false;
}

void status_text(char* buf, size_t n) {
    std::snprintf(buf, n, " shoulder_shifted=%llu shoulder_walled=%llu", static_cast<unsigned long long>(g.shifted.load()),
                  static_cast<unsigned long long>(g.limited.load()));
}

}  // namespace

const Feature kShoulderCameraFeature = {
    .name = "shoulder_camera", .install = &install, .remove = &remove, .game_frame = &game_frame, .status_text = &status_text};

}  // namespace nxrt::games::pkp
