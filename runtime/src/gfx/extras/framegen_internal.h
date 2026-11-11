// SPDX-License-Identifier: GPL-3.0-or-later
// What the two files of the FSR 3.1 frame generation share (framegen_ffx.cpp: the swapchain; framegen_ffx_frame.cpp: the frames).
#pragma once
#include <windows.h>

#include "nxrt/gfx.h"

#include "../../../third_party/amd_ffx_api/api/include/ffx_api.h"
#include "../../../third_party/amd_ffx_api/api/include/ffx_api_types.h"
#include "../../../third_party/amd_ffx_api/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/amd_ffx_api/framegeneration/include/ffx_framegeneration.h"
#include "../../../third_party/amd_ffx_api/framegeneration/include/vk/ffx_api_framegeneration_vk.h"

namespace nxrt::extras::fg {

constexpr uint32_t kHudlessRing = 3;   // the copy of a frame stays until its presentation is done: as many as frames in flight

struct State {
    gfx::Device* dev = nullptr;
    HMODULE mod = nullptr;
    PfnFfxCreateContext create_ctx = nullptr;
    PfnFfxDestroyContext destroy_ctx = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    // the swapchain's context, its replacement calls, and `native`: AMD's code could not make the first swapchain, everything forwards to Vulkan
    ffxContext sc_ctx = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK fns = {};
    bool native = false;
    VkSwapchainKHR live = VK_NULL_HANDLE;       // the swapchain AMD's code made and has not been told to destroy
    VkExtent2D extent = {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    gfx::SwapchainFns table;
    // the frame generation context (made at the first frame with a camera: it needs the render size and the depth convention)
    ffxContext ctx = nullptr;
    VkExtent2D render = {};
    bool depth_inverted = false;
    bool failed = false;                        // the context could not be made or a call failed: frames are presented without generation
    uint64_t frame_id = 0;                      // +1 per presented frame
    bool prepared = false;                      // the frame being built had a camera: the prepare dispatch was recorded
    gfx::Image hudless[kHudlessRing];
    uint32_t hudless_next = 0;
    int hudless_current = -1;                   // the ring slot copied for the frame being built, -1: none
};

State& state();

// The frame side (framegen_ffx_frame.cpp): called by the swapchain side.
void frame_before_present();                    // configures the frame generation context for the frame about to be presented
void frame_destroy();                           // destroys the context and the images (before the swapchain goes)

}  // namespace nxrt::extras::fg
