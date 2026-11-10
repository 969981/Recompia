// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA DLSS / DLAA left out of the build (NXRT_UPSCALER_DLSS=OFF, cmake/gfx.cmake): the temporal.h backend reports itself unavailable,
// so temporal_evaluate records nothing in those modes, and Vulkan starts on the normal loader.
#include "extras/temporal.h"

namespace nxrt::extras {

bool temporal_dlss_available() { return false; }
bool temporal_dlss_started() { return false; }
PFN_vkGetInstanceProcAddr streamline_preinit() { return nullptr; }
TemporalBackend* temporal_dlss(void*, bool) { return nullptr; }

}  // namespace nxrt::extras
