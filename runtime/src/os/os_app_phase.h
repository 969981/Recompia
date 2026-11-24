// SPDX-License-Identifier: GPL-3.0-or-later
// os_app_phase.h: start-up phases for the app that hosts the runtime. On Android the activity shows its own loading screen from Play
// until the first picture (the screen is otherwise black while the game boots) and hides it on kPicture; the phases go to its UI
// thread with SDL_SendAndroidMessage (thread-safe, does not wait; game-build/android .../LoadingScreen.kt: COMMAND and PHASE_*).
// Desktop: nothing (the window opens with the first present).
#pragma once

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#endif

namespace nxrt::os::app {

enum Phase : int {
    kShaders = 1,        // the pipeline prewarm started: shaders load while the game boots
    kFirstPresent = 2,   // the game presents (its first frames are often black full-screen passes while it loads)
    kPicture = 3,        // the first present after a blended draw into a presented texture: the app's loading screen goes
};

inline void phase(Phase p) {
#if defined(__ANDROID__)
    SDL_SendAndroidMessage(0x8001, p);   // SDLActivity.COMMAND_USER + 1, handled by GameActivity.onUnhandledMessage
#else
    (void)p;
#endif
}

}  // namespace nxrt::os::app
