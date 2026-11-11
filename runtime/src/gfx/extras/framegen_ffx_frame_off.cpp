// SPDX-License-Identifier: GPL-3.0-or-later
// The frames of AMD FSR 3.1 frame generation left out of the build (NXRT_UPSCALER_FSR3=OFF, cmake/gfx.cmake): nothing is generated.
#include "extras/framegen.h"

namespace nxrt::extras {

bool framegen_active() { return false; }

void framegen_prepare(VkCommandBuffer, const TemporalFrame&) {}

bool framegen_hudless_capture(VkCommandBuffer, VkImage, VkImageLayout, VkExtent2D) { return false; }

}  // namespace nxrt::extras
