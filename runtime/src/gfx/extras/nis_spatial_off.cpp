// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA Image Scaling left out of the build (NXRT_UPSCALER_NIS=OFF, cmake/gfx.cmake): init fails, so the output pass and the scene
// upscaler use FSR 1 as they do when the NIS pipeline cannot be made.
#include "extras/nis_spatial.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-private-field"   // the class's state is only used by the real nis_spatial.cpp
#endif

namespace nxrt::extras {

bool NisSpatial::init(gfx::Device&) { return false; }
void NisSpatial::shutdown() {}
bool NisSpatial::supports(VkExtent2D, VkExtent2D) const { return false; }
VkImageView NisSpatial::upscale(VkCommandBuffer, VkImageView, VkExtent2D, VkExtent2D, VkExtent2D, bool, float, float, VkExtent2D) { return VK_NULL_HANDLE; }

}  // namespace nxrt::extras
