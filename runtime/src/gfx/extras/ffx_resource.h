// SPDX-License-Identifier: GPL-3.0-or-later
// A Vulkan image as the FidelityFX API names it, for the two users of the API: temporal_ffx.cpp (upscaling) and framegen_ffx_frame.cpp (frame
// generation).
#pragma once
#include <volk.h>

#include <cstdint>

#include "../../../third_party/amd_ffx_api/api/include/ffx_api_types.h"

namespace nxrt::extras {

inline uint32_t ffx_surface_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
        case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

inline FfxApiResource ffx_resource(VkImage image, VkFormat format, uint32_t width, uint32_t height, uint32_t state, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = reinterpret_cast<void*>(image);
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_surface_format(format);
    r.description.width = width;
    r.description.height = height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}

}  // namespace nxrt::extras
