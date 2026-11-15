// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode's PC features (pkp_play.cpp and the files it is split into): the state and the hook sites they share, and the
// functions one of them calls in another. Internal to those files.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_hook_set.h"
#include "core/pkp_layout.h"
#include "play_game.h"

namespace nxrt::games::pkp::play_detail {

constexpr const char* kTag = "pkp";

using guest::rd;
using guest::wr;

// ---- offsets in main ----
constexpr uint64_t kLevelWidth = 0x1129990, kLevelHeight = 0x11299d0;       // u32 (u8 level)
constexpr uint64_t kFullWidth = 0x1129980, kFullHeight = 0x11299c0;         // u32 (): 1920 / 1080
constexpr uint64_t kCopyParams = 0x22d1e0;                                  // dynres manager: copy the frame's size parameters (x0 = manager)
constexpr uint64_t kReplayViewports = 0x17b6c98;                            // mid: render-thread pass replay, before SetViewports
constexpr uint64_t kSetViewport = 0x17b50ac, kSetViewports = 0x17b5254;     // mid: the command context's SetViewport (x1 rect) / SetViewports (w1 n, x2 rects)
constexpr uint64_t kRecordViewports = 0x17c0420;                            // entry: records viewports into the pass state (x0 ctx + 0x10, w1 n, x2 rects)
constexpr uint64_t kCreateTarget = 0x1796810;                               // entry: KTGL 2D render-target create (w1 width, w2 height, w4 format)
constexpr double kGameMaxFps = 60.0;                                        // the frame logic's design rate (event hold)
constexpr uint64_t kFrameMultiplier = layout::kFrameStep;                   // mid: s0 multiplier, s8 frame time
constexpr uint64_t kTweenStep[4] = {0xe51688, 0xe516f0, 0xe5173c, 0xe51788};   // mid: general tween step (fadd s0, s0, s1): x19 tween, x10 counter, s0 multiplier, s1 0.5
constexpr uint64_t kSyncPeriod = 0x89e27c;                                  // mid: x22 the Sync wait's frame period in ticks (frequency / 60)
constexpr uint64_t kFixedStepRate = 0x89e398;                               // mid: s0 60.0, x28 now, x25 previous frame tick, d8 tick frequency
constexpr uint64_t kCamStick = 0x87d30;                                     // mid: x19 camera, s0 right stick x
constexpr uint64_t kCamField = 0x87cd0;                                     // mid: x19 camera, the update past its scene check, before the turn gate
constexpr uint64_t kFollowGate = 0x249848;                                  // mid: w0 bit 0 = run the follow placement (0x783c0) this frame
constexpr uint64_t kViewsGlobal = 0x3561880;                                // object whose byte +562 says two views (split screen)
using layout::kSaveRoot;
constexpr int kFullLevel = 30;
constexpr int kTableW = 1920, kTableH = 1080;

// dynres manager layout: per view (stride 28) +16 target w/h, +24 largest scene w/h, +32 scene w/h, +40 levels (target, largest, current)
constexpr size_t kViewStride = 28, kTgtW = 16, kTgtH = 20, kMaxW = 24, kMaxH = 28, kCurW = 32, kCurH = 36, kLvlMax = 41, kLvlCur = 42;

// follow camera layout (+0xcc / +0xd0: the stick's pitch / yaw rate, radians per second at full deflection)
constexpr size_t kCamPlayer = 8, kCamPitch = 0xa0, kCamYaw = 0xa4, kCamPitchRate = 0xcc, kCamYawRate = 0xd0, kCamPitchMax = 0xd4,
                 kCamPitchMin = 0xd8, kCamManual = 0xff, kCamTwinPitch = 0x124, kCamTwinYaw = 0x128, kCamIdle = 0x13c;
// the camera manager: the embedded follow camera of local player i at +0x10 + i * 0x1b0
constexpr uint64_t kCameraManager = 0x3561948;   // qword
constexpr size_t kMgrFollow = 0x10, kMgrFollowStride = 0x1b0;
// photo camera (camera mode 3, pkp_photo_camera.cpp): the turn function 0x8ee4c (x19 = the photo camera, s8 = the frame time)
constexpr uint64_t kPhotoStickX = 0x8eef0;       // mid: s10 = the right stick's x, right after its load
constexpr uint64_t kPhotoStickY = 0x8f1e4;       // mid: s0 = the right stick's y, right after its load
constexpr uint32_t kPhotoSiteWord = 0x1e2a1001;  // `fmov s1, #0.25` at both sites (the turn's dead zone)
constexpr size_t kPhotoPlayer = 8, kPhotoInvertX = 9, kPhotoInvertY = 10;   // u8: local player index; the game's invert options
constexpr double kTwoPi = 6.283185307179586;
constexpr float kHalfPi = 1.5707964f;

struct State {
    uint64_t base = 0x7100000000ull;
    play::GameHost* host = nullptr;
    std::atomic<int> w{kTableW}, h{kTableH};
    std::atomic<bool> dynres{false};
    std::atomic<bool> fps_fix{true};
    std::atomic<bool> hold_on{true};          // [pkp] event_hold
    uint64_t hold_ns = 1500000000ull;         // [pkp] event_hold_ms
    std::atomic<uint64_t> seq_ns{0};          // the last frame out of free play (or loading)
    bool holding = false;                     // main thread: the 60 fps ceiling is set
    uint64_t hold_start_ns = 0, holds = 0;
    std::atomic<uint64_t> turn_ns{0};         // the last frame the player could turn the camera (free play)
    std::atomic<bool> game_limit{false};
    std::atomic<uint64_t> sync_calls{0}, fixed_calls{0};
    std::atomic<bool> mouse_cam{true};
    std::atomic<bool> cam_every_frame{true};
    uint64_t grace_ns = 250000000ull;
    // main thread (the follow camera's update): counts read before the turn branch and not applied yet
    double pend_dx = 0, pend_dy = 0;
    uint64_t last_turn_ns = 0;
    std::atomic<uint64_t> cam_field{0}, follow_forced{0}, cam_dropped{0};
    // main thread (the photo camera's turn, pkp_photo_camera.cpp): mouse turn not handed to the game yet, radians (the stick's dead zone)
    double photo_yaw = 0, photo_pitch = 0;
    int photo_sites = 0;                      // 0 = not checked yet, 1 = main has the expected words, -1 = not: the photo hooks stay off
    std::atomic<uint64_t> photo_calls{0}, photo_moves{0};
    HookSet* hooks = nullptr;                // the registry's set for play (pkp_feature.h): every hook of these files goes in it
    std::atomic<uint64_t> res_calls{0}, copy_calls{0}, vp_fixes{0}, fps_calls{0}, cam_calls{0}, cam_moves{0};
    std::atomic<uint64_t> vp_shapes[16]{};   // distinct scissor sizes the viewport stretch has touched (logged once each)
    std::atomic<uint32_t> vp_shape_n{0};
    std::atomic<float> last_mult{1.f};
    // #275: the game thread's average frame time (EMA of the frame multiplier hook's s8) and whether that is above 60 fps; only then do
    // frames shorter than 1/60 s get a fractional step (hook_frame_multiplier, hook_fixed_step_rate)
    float avg_dt = 1.f / 60.f;
    std::atomic<bool> above60{false};
    // hook_tween_step: per tween object, the counter and step seen last time and the carried fraction of a 60 Hz frame
    struct TweenAcc { uint64_t obj = 0; uint32_t last = 0, step = 0; float acc = 0.f; };
    std::mutex tween_mu;
    TweenAcc tween[64];
    uint32_t tween_next = 0;
    // Joy-Con 2 mouse mode (frame boundary, main thread): the Mouse mode key flips `mm_want`; the activation count tells how often it fired
    uint64_t mm_seen = 0;
    bool mm_want = false;
    uint64_t mm_log_ns = 0;
    std::atomic<uint64_t> mm_dropped{0};
    std::atomic<bool> mm_gate{false};        // the game's mouse mode is let through its "two Joy-Cons" check (hook_mm_style)
    uint64_t mm_gate_logs = 0;
    uint64_t mm_exit_ns = 0;                 // the key turned mouse mode off at this time while the game's flag was still on (0 = not leaving)
    uint64_t mm_diag_ns = 0, mm_diag_logs = 0;
    bool render_installed = false, installed = false;
};

State& st();

// ---- Joy-Con 2 mouse mode key, play mode's frame (pkp_mouse_mode.cpp) ----
void play_frame(bool in_game, double step_seconds, play::GameFrame* out);

// ---- render resolution (pkp_render_size.cpp) ----
int hook_copy_params(::Ctx* c, uint64_t, void*);
extern std::atomic<uint64_t> g_set_vp_fixes;
void install_render_size_hooks(uint64_t base);
void pick_size(const play::DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h);
void game_target_memory(uint64_t base, int* w, int* h);

// ---- render targets created at a literal size (pkp_render_targets.cpp) ----
enum class TargetFix : uint8_t { Off, Screen, All };
extern std::atomic<uint8_t> g_target_fix;
TargetFix target_fix_from_config();
int hook_create_target(::Ctx* c, uint64_t, void*);

// ---- aspect ratio (pkp_aspect.cpp) ----
constexpr uint64_t kCamSubmit = 0x8cc124;   // mid: x20 = the camera whose view-projection goes to the render view
constexpr uint64_t kProjBuild = 0x1f12130;  // mid: x0 = the camera, the projection builder after its prologue
constexpr uint64_t kUiCamera = 0x1fbab98;   // mid: x2 = int[2] screen size of the layout camera
constexpr double kAspect169 = 16.0 / 9.0;

struct Wide {
    double aspect = kAspect169;               // the display's aspect (16:9 = nothing to do)
    std::atomic<uint64_t> cams[8]{};          // scene cameras seen at the render-view hand-over
    std::atomic<uint32_t> fov_put[8]{};       // [pkp] taller_fov: the FOV bits last written into cams[i] (0 = none)
    bool taller_fov = true;                   // [pkp] taller_fov (default on): narrower than 16:9 through the camera's FOV and aspect, not the matrix only
    std::atomic<uint64_t> cam_fixes{0}, aspect_puts{0}, fov_puts{0}, ui_fixes{0};
    std::atomic<bool> fov_logged{false};
    uint64_t ui_size = 0;                     // guest int[2] the layout camera reads instead of 1920x1080
};
Wide& wide();
bool close_to(double a, double b);
bool wide_on();
int hook_cam_submit(::Ctx* c, uint64_t, void*);
int hook_proj_build(::Ctx* c, uint64_t, void*);
int hook_ui_camera(::Ctx* c, uint64_t, void*);

// ---- wide text spacing (pkp_wide_text.cpp) ----
constexpr uint64_t kTextSpacing = 0x1f779d0, kTextWriterStyle = 0x1f877b0;
int hook_text_spacing(::Ctx* c, uint64_t, void*);
int hook_text_writer_style(::Ctx* c, uint64_t, void*);

// ---- frame rate (pkp_frame_rate.cpp) ----
int hook_frame_multiplier(::Ctx* c, uint64_t, void*);
int hook_tween_step(::Ctx* c, uint64_t, void*);
int hook_sync_period(::Ctx* c, uint64_t, void*);
int hook_fixed_step_rate(::Ctx* c, uint64_t, void*);

// ---- mouse camera (pkp_mouse_camera.cpp) ----
uint64_t mono_ns();
extern std::atomic<uint64_t> g_follow_ns;
int hook_cam_field(::Ctx*, uint64_t, void*);
int hook_follow_gate(::Ctx* c, uint64_t, void*);
int hook_cam_stick(::Ctx* c, uint64_t, void*);

// ---- photo camera (pkp_photo_camera.cpp) ----
extern std::atomic<uint64_t> g_photo_ns;
int hook_photo_stick_x(::Ctx* c, uint64_t, void*);
int hook_photo_stick_y(::Ctx* c, uint64_t, void*);

// ---- Joy-Con 2 mouse mode (pkp_mouse_mode.cpp) ----
// Joy-Con 2 mouse mode gate (see "Joy-Con 2 mouse mode" in pkp_mouse_mode.cpp)
constexpr uint64_t kMmStyle = 0x2fa6f8;       // mid: the check right after it asked the pad style (w0, 1 = two Joy-Cons; x19 = character)
constexpr uint64_t kMmOption = 0x2fa8a0;      // mid: the check's last step (w0 = the save's mouse option bit; x19 = character)
constexpr uint64_t kMmStart = 0x2fa8ec;       // mid: mouse mode turning on, at the option test (w8 = the option byte; x19 = character)
int hook_mm_style(::Ctx* c, uint64_t, void*);
int hook_mm_option(::Ctx* c, uint64_t, void*);
int hook_mm_start(::Ctx* c, uint64_t, void*);

}  // namespace nxrt::games::pkp::play_detail
