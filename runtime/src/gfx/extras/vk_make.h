// SPDX-License-Identifier: GPL-3.0-or-later
// Small Vulkan object creators shared by the full-screen passes of extras/ (output_pass, overlay_composite), each with the vk_gauges.h bookkeeping.
#pragma once
#include <volk.h>

#include <cstddef>
#include <cstdint>

#include "vk_gauges.h"

namespace nxrt::extras {

inline VkShaderModule make_module(VkDevice d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_gauge_shader_modules().add(1);
    return m;
}

// A sampler with one filter for magnification and minification, no mip filtering and clamp-to-edge addressing.
inline bool make_clamp_sampler(VkDevice d, VkFilter filter, VkSampler* out) {
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d, &sci, nullptr, out) != VK_SUCCESS) return false;
    vk_gauge_samplers().add(1);
    return true;
}

// A descriptor pool for `sets` sets of one combined image sampler each.
inline bool make_sampler_pool(VkDevice d, uint32_t sets, VkDescriptorPool* out) {
    const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = sets;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, out) != VK_SUCCESS) return false;
    vk_gauge_descriptor_pools().add(1);
    return true;
}

}  // namespace nxrt::extras
