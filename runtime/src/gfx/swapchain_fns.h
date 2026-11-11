// SPDX-License-Identifier: GPL-3.0-or-later
// The calls the renderer makes on its swapchain (vk_renderer.cpp), as a table. They are the Vulkan ones; an extra that replaces the swapchain
// (AMD's frame-generation swapchain, extras/framegen.h) hands the renderer its own set with the same signatures, so the renderer has one
// swapchain path and no knowledge of who made the swapchain.
#pragma once
#include "nxrt/gfx.h"

namespace nxrt::gfx {

struct SwapchainFns {
    PFN_vkCreateSwapchainKHR create = vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR destroy = vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR images = vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR acquire = vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR present = vkQueuePresentKHR;
    PFN_vkSetHdrMetadataEXT hdr_metadata = vkSetHdrMetadataEXT;
};

}  // namespace nxrt::gfx
