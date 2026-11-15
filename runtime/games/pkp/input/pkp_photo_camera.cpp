// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: the mouse turns the photo camera (pkp_play.h; part of play mode's PC features). Offsets in `main` of the 2.0.1 update.
//
// Photo mode (the Camera from the main menu or the tool wheel: 0x259bc0 sets the player's flow state 7) switches the camera manager
// (0x3561948) to mode 3: byte +0xcc8 + player = 3, the camera at +0x730 + player * 0xf0 (class vtable 0x322ce40, update 0x8e660). The
// follow camera's update (0x87840) does not run then, so the mouse camera hooks (pkp_mouse_camera.cpp) never tell play mode the game is
// in play: the frame was a menu frame (no capture, the menu pointer, the counts dropped). While the photo camera takes look input its
// update calls the turn function 0x8ee4c (from 0x8eaf0, state +0xe0 = 0): it reads the right stick (pad state + player * 0x120 + 0x278 /
// +0x27c), ignores |x| <= 0.25 (the same for y), and turns by stick * the follow camera's rate (+0xd0 yaw, +0xcc pitch, radians per
// second) * the frame time (s8): yaw +0x84 -= x (sub-mode +0x94 = 2 also +0xc4, clamped), pitch +0x80 += y (negated in sub-modes 1 and 2),
// clamped to the photo tuning limits; +9 / +10 are the game's invert options.
// The mid hooks right after the two stick loads are this module's signal and its way in: the x hook reports the input tick (play mode then
// captures the mouse and hands over the counts, as for the follow camera), and both add the mouse turn to the stick value the game reads,
// so the game's own sub-modes, clamps and character turn apply. The turn is exact radians per count from [mouse] yaw / pitch (the stick
// value is the turn divided by rate * frame time), with the player's [mouse] invert and the game's invert options cancelled, like the
// follow camera's mouse turn. A turn below the game's dead zone waits until it adds up to one past it (at most 0.25 * rate * frame time,
// well under a degree), and a turn past kMaxStick waits for the next frame, so nothing the hand did is lost; a wait older than the grace
// time is dropped (photo mode left and entered again).
#include "input/pkp_play_internal.h"

#include <algorithm>
#include <cmath>

#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp::play_detail {

std::atomic<uint64_t> g_photo_ns{0};   // the last photo-camera turn (play_photo_live)

namespace {

constexpr float kDeadZone = 0.25f;      // the turn function's own threshold (both axes)
constexpr float kMaxStick = 8.f;        // the most one frame hands over (8 full sticks); the game's turn is linear in the value

bool sites_ok(State& s) {
    if (s.photo_sites == 0) {
        const bool ok = rd<uint32_t>(s.base + kPhotoStickX) == kPhotoSiteWord && rd<uint32_t>(s.base + kPhotoStickY) == kPhotoSiteWord;
        s.photo_sites = ok ? 1 : -1;
        if (ok) NXLOG_I(kTag, "photo camera: the mouse turns it (first photo-mode turn)");
        else NXLOG_W(kTag, "photo camera: off: main+0x%llx / main+0x%llx are not the expected hook sites", static_cast<unsigned long long>(kPhotoStickX),
                     static_cast<unsigned long long>(kPhotoStickY));
    }
    return s.photo_sites > 0;
}

// The follow camera of the photo camera's player: its stick rates are the photo camera's too. 0 when unknown.
uint64_t follow_of(const State& s, uint64_t photo) {
    if (!photo || !mem::is_mapped(photo, kPhotoInvertY + 1)) return 0;
    const uint8_t player = rd<uint8_t>(photo + kPhotoPlayer);
    const uint64_t mgr = rd<uint64_t>(s.base + kCameraManager);
    if (player > 1 || !mgr) return 0;
    const uint64_t follow = mgr + kMgrFollow + player * kMgrFollowStride;
    return mem::is_mapped(follow, kCamYawRate + 4) ? follow : 0;
}

// The stick value `stick` with `*pend` radians of turn added, at `rate` radians per second per full stick over frame time dt. Hands over
// what passes the dead zone (up to kMaxStick) and keeps the rest in *pend; returns the value for the game.
float add_turn(float stick, double* pend, float rate, float dt, bool game_inverts) {
    if (*pend == 0 || !(rate > 0.f) || !(dt > 0.f)) return stick;
    const double per_stick = static_cast<double>(rate) * dt * (game_inverts ? -1.0 : 1.0);   // radians the game turns per unit of stick
    double add = *pend / per_stick;
    const double total = std::clamp(static_cast<double>(stick) + add, -static_cast<double>(kMaxStick), static_cast<double>(kMaxStick));
    if (!(total > kDeadZone || total <= -kDeadZone)) return stick;                           // the game would ignore it: keep waiting
    add = total - stick;
    *pend -= add * per_stick;
    if (std::fabs(*pend) < 1e-9) *pend = 0;
    return static_cast<float>(total);
}

}  // namespace

// x: s10 = the stick's x. The photo camera takes look input this frame: the game is in play (input tick); the counts become the turn.
int hook_photo_stick_x(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.host || !sites_ok(s)) return 0;
    s.photo_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = mono_ns();
    const uint64_t last = g_photo_ns.exchange(now, std::memory_order_relaxed);
    if (last == 0 || now - last > s.grace_ns) s.photo_yaw = s.photo_pitch = 0;   // a new photo session: nothing from the last one
    double dx = 0, dy = 0;
    s.host->input_tick(&dx, &dy);
    if (hid::jc2_mouse_active()) {                  // the Joy-Con 2 mouse has the mouse: it is not the camera's
        if (dx != 0 || dy != 0) s.mm_dropped.fetch_add(1, std::memory_order_relaxed);
        dx = dy = 0;
    }
    if (!s.mouse_cam.load(std::memory_order_relaxed)) dx = dy = 0;
    if (dx != 0 || dy != 0) {
        const play::MouseSettings& m = s.host->settings().mouse;
        // as the follow camera: mouse right = stick right, mouse dy = stick y
        s.photo_yaw += dx * m.yaw_rad * (m.invert_x ? -1.0 : 1.0);
        s.photo_pitch += dy * m.pitch_rad * (m.invert_y ? -1.0 : 1.0);
        s.host->aim_applied(dx, dy);
    }
    const uint64_t cam = c->x[19];
    const uint64_t follow = follow_of(s, cam);
    if (!follow) return 0;
    const float before = c->v[10].f[0];
    c->v[10].f[0] = add_turn(before, &s.photo_yaw, rd<float>(follow + kCamYawRate), c->v[8].f[0], rd<uint8_t>(cam + kPhotoInvertX) != 0);
    if (c->v[10].f[0] != before) s.photo_moves.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// y: s0 = the stick's y (the same call of the turn function, after the x part).
int hook_photo_stick_y(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.host || s.photo_sites <= 0) return 0;
    const uint64_t cam = c->x[19];
    const uint64_t follow = follow_of(s, cam);
    if (!follow) return 0;
    c->v[0].f[0] = add_turn(c->v[0].f[0], &s.photo_pitch, rd<float>(follow + kCamPitchRate), c->v[8].f[0], rd<uint8_t>(cam + kPhotoInvertY) != 0);
    return 0;
}

}  // namespace nxrt::games::pkp::play_detail
