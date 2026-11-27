// SPDX-License-Identifier: GPL-3.0-or-later
// Phone CPU placement (src/os/os_android_perfhint.cpp). The guest's core pinning does nothing on Android (SetThreadAffinityMask is a
// no-op there), so the game's busy threads float over all cores and the scheduler, seeing each of them at 20-40 % load, keeps them on the
// little cores and the clocks low while the frame waits on them. [sched] phone_hint (default on) gives them an Android performance hint
// session (ADPF, API 33+), told the frame target and each frame's real interval, so the platform raises their clocks and core class only
// as far as the frame needs. Off Android this does nothing.
#pragma once

#include <cstdint>

namespace nxrt::perfhint {

#if defined(__ANDROID__)
void frame(uint64_t interval_ns);   // once per presented frame, from the presenting thread (starts the monitor on the first call)
#else
inline void frame(uint64_t) {}
#endif

}  // namespace nxrt::perfhint
