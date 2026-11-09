// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: the raw mouse camera (pkp_play.h; split out of pkp_play.cpp). Offsets in `main` of the 2.0.1 update.
//
// Mouse camera. The follow camera's update (0x87840, vtable slot of the class at 0x322cd58) turns the right stick (pad state +0x278 / +0x27c)
// into its yaw (+0xa4, radians, 0..2pi, stick right lowers it) and pitch (+0xa0, clamped to [+0xd8, +0xd4]), with the twin angles +0x128
// / +0x124 when the strafe style option is on, and marks manual input (+0xff = 1, +0x13c = 0: the auto-adjust delay). The mid hook right
// after it read the stick (0x87d30, inside the branch that runs only while the player may turn the camera) adds the mouse counts the same
// way: exact radians per count from [mouse] yaw / pitch, independent of the game's camera speed.
// What the game does with the angles (IDA): each frame runs the camera manager's update (0x71000b80c0 -> the update above), then
// 0x7100249780, then the view write (0x7100083b40 -> 0x7100088aac copies the eye +0x60 / target +0x50 into the view block). There is no
// easing on yaw or pitch: the follow placement 0x7100078440 (via 0x71000783c0) builds the eye straight from them (0x710007bef0: target +
// R(pitch, yaw) * distance), pushes it in for collisions and writes the angles back from the result; its recentre helpers (0x710007c0c0,
// 0x710007d720) stay off while +0xff (manual) is set. But 0x7100249780 runs that placement only when its tick (0x7100249960) fires, and the
// tick fires when the summed frame time passes 1/120 s, then restarts from 0: above 120 fps the eye moved every second or third frame (at
// ~185 fps about 92 times a second, unevenly) while the angles moved every frame, and the placement's own time steps (it gets the frame
// time, not the summed one) ran slow. The mid hook after that tick (0x7100249848, w0 bit 0 = run the placement) runs the placement every
// frame ([pkp] camera_every_frame, default 1); the tick's other work (collision lists) keeps its own rate.
// The turn branch (0x87d30) is closed whenever the game's own stick is ignored (+0x111 / +0x112 flags: player state 0x88, scene conditions;
// 0x7100300a6c; 0x71000887f0). It was also the only place that told play mode the game is in play, and play mode trusts that for 4 frames
// (21 ms at 185 fps): a short closure dropped the context to the menu (cursor released and shown, counts discarded). The mid hook at 0x87cd0
// (every follow-camera update past its scene check, before the gate) keeps reading the mouse for [pkp] camera_grace_ms (default 250) after
// the branch last ran: counts read while the branch is closed are applied when it opens again within that time (the view ends where the
// hand moved it) and dropped after it (a scripted camera does not jump when control comes back).
#include "pkp_play_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "nxrt/jc2_mouse.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp::play_detail {

// ---- mouse camera ----
float wrap_angle(float a) {
    if (a > static_cast<float>(kTwoPi)) a -= static_cast<float>(kTwoPi);
    if (a < 0.f) a += static_cast<float>(kTwoPi);
    return a;
}

// The strafe style option the camera reads through 0x27abc8 (player 1 reads its own block, the others the shared one).
constexpr uint64_t kSaveStrafeP1 = 0x1718b64;   // save data: u8, player 1's strafe style (nonzero = on)
constexpr uint64_t kSaveStrafe = 0x6cfdd8;      // save data: u8, bit 1 = the shared block's strafe style
bool twin_angles(uint8_t player) {
    const uint64_t save = rd<uint64_t>(st().base + kSaveRoot);
    if (!save) return false;
    if (player == 1) return mem::is_mapped(save + kSaveStrafeP1, 1) && rd<uint8_t>(save + kSaveStrafeP1) != 0;
    return mem::is_mapped(save + kSaveStrafe, 1) && (rd<uint8_t>(save + kSaveStrafe) & 2) != 0;
}

uint64_t mono_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic<uint64_t> g_follow_ns{0};   // the last follow-camera update (play_follow_live)

// Every follow-camera update past its scene check: while the turn branch ran within the grace time, read the mouse here (this keeps play mode's
// "in game", and so the capture, through short closures of the branch) and keep the counts for the branch; after it, drop what was kept.
int hook_cam_field(::Ctx*, uint64_t, void*) {
    State& s = st();
    if (!s.host) return 0;
    s.cam_field.fetch_add(1, std::memory_order_relaxed);
    g_follow_ns.store(mono_ns(), std::memory_order_relaxed);
    if (s.last_turn_ns == 0 || mono_ns() - s.last_turn_ns > s.grace_ns) {
        if (s.pend_dx != 0 || s.pend_dy != 0) s.cam_dropped.fetch_add(1, std::memory_order_relaxed);
        s.pend_dx = s.pend_dy = 0;
        return 0;
    }
    double dx = 0, dy = 0;
    s.host->input_tick(&dx, &dy);                   // the first call after the frame boundary takes the counts (late sample), later ones get 0
    if (hid::jc2_mouse_active()) {                  // the Joy-Con 2 mouse has the mouse: it is not the camera's
        if (dx != 0 || dy != 0) s.mm_dropped.fetch_add(1, std::memory_order_relaxed);
        dx = dy = 0;
    }
    s.pend_dx += dx;
    s.pend_dy += dy;
    return 0;
}

// The follow placement runs every frame instead of on the 120 Hz tick, so the eye moves with the angles on every frame.
int hook_follow_gate(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.cam_every_frame.load(std::memory_order_relaxed)) return 0;
    if ((c->x[0] & 1) == 0) s.follow_forced.fetch_add(1, std::memory_order_relaxed);
    c->x[0] |= 1;
    return 0;
}

int hook_cam_stick(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.host) return 0;
    s.cam_calls.fetch_add(1, std::memory_order_relaxed);
    double dx = 0, dy = 0;
    s.host->input_tick(&dx, &dy);                   // the game is in play: play mode captures the mouse and hands over the counts (0 when the field hook took them)
    dx += s.pend_dx;
    dy += s.pend_dy;
    s.pend_dx = s.pend_dy = 0;
    if (hid::jc2_mouse_active()) {                  // the Joy-Con 2 mouse has the mouse: drop the counts
        if (dx != 0 || dy != 0) s.mm_dropped.fetch_add(1, std::memory_order_relaxed);
        dx = dy = 0;
    }
    s.last_turn_ns = mono_ns();
    s.turn_ns.store(s.last_turn_ns, std::memory_order_relaxed);
    if (!s.mouse_cam.load(std::memory_order_relaxed) || (dx == 0 && dy == 0)) return 0;
    const uint64_t cam = c->x[19];
    if (!cam || !mem::is_mapped(cam, kCamIdle + 4)) return 0;
    const play::MouseSettings& m = s.host->settings().mouse;
    // mouse right = stick right (the yaw goes down); mouse up = stick up (the pitch goes up)
    const float dyaw = static_cast<float>(dx * m.yaw_rad * (m.invert_x ? -1.0 : 1.0));
    const float dpitch = static_cast<float>(dy * m.pitch_rad * (m.invert_y ? -1.0 : 1.0));
    wr<float>(cam + kCamYaw, wrap_angle(rd<float>(cam + kCamYaw) - dyaw));
    const bool twin = twin_angles(rd<uint8_t>(cam + kCamPlayer));
    if (twin) wr<float>(cam + kCamTwinYaw, wrap_angle(rd<float>(cam + kCamTwinYaw) - dyaw));
    if (dpitch != 0.f) {
        float p = rd<float>(cam + kCamPitch) + dpitch;
        p = std::min(p, rd<float>(cam + kCamPitchMax));
        p = std::max(p, rd<float>(cam + kCamPitchMin));
        wr<float>(cam + kCamPitch, p);
        if (twin) wr<float>(cam + kCamTwinPitch, std::clamp(rd<float>(cam + kCamTwinPitch) + dpitch, -kHalfPi, kHalfPi));
    }
    wr<uint32_t>(cam + kCamIdle, 0);
    wr<uint8_t>(cam + kCamManual, 1);
    s.cam_moves.fetch_add(1, std::memory_order_relaxed);
    s.host->aim_applied(dx, dy);
    return 0;
}

}  // namespace nxrt::games::pkp::play_detail
