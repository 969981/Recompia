// SPDX-License-Identifier: GPL-3.0-or-later
// Facts about the platform the runtime is built for, known at compile time and chosen here, in one place (the only per-OS branch
// outside src/os/). Named by what they mean, not by the OS, so a call site says why it differs: `if constexpr (facts::kPhone)`.
// A desktop build compiles none of the code behind a false fact; host_is_handheld() and friends (nxrt/host_device.h) are the
// run-time counterparts for facts only the running machine knows.
#pragma once

namespace nxrt::facts {

#if defined(__ANDROID__)
inline constexpr bool kOnAndroid = true;
#else
inline constexpr bool kOnAndroid = false;
#endif

// A phone or tablet: factory defaults that suit its CPU and window system (a 30 fps limit, borderless) and no spinning waits (a
// spinning big core heats the SoC and lowers the thermal limits of the GPU and every thread). A player's own choice is never changed.
inline constexpr bool kPhone = kOnAndroid;
// The OS stops the app while it is in the background (SDL's pump blocks the game's main thread until it is back): no frame is due
// then, so neither the freeze watchdog nor the hitch log counts that time.
inline constexpr bool kAppStoppedInBackground = kOnAndroid;
// The window's native surface is destroyed in the background and replaced on return, and its reported extent can lag behind the
// window (a fold or unfold): the renderer makes the Vulkan surface again and retries one it cannot present to.
inline constexpr bool kSurfaceReplaceable = kOnAndroid;
// The surface can be rotated against the picture (a phone whose natural orientation is portrait, held landscape): the swapchain asks
// for IDENTITY and lets the presentation engine rotate, so VK_SUBOPTIMAL_KHR is expected and no reason to rebuild.
inline constexpr bool kSurfaceMayRotate = kOnAndroid;
// The window is locked to landscape: a taller than wide window is a moment of the activity turning (at start), not a shape to render for.
inline constexpr bool kLandscapeOnly = kOnAndroid;

}  // namespace nxrt::facts
