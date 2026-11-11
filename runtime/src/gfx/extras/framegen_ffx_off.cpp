// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 frame generation left out of the build (NXRT_UPSCALER_FSR3=OFF, cmake/gfx.cmake): it reports itself unavailable.
#include "extras/framegen.h"

namespace nxrt::extras {

bool framegen_dll_found() { return false; }

const gfx::SwapchainFns* framegen_swapchain(gfx::Device&) { return nullptr; }

bool framegen_present_count(uint64_t*) { return false; }

const char* framegen_missing_text() { return "Frame generation is not part of this build."; }

}  // namespace nxrt::extras
