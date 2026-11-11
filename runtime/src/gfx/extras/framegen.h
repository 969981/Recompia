// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 frame generation through the FidelityFX API (framegen_ffx.cpp; framegen_ffx_off.cpp when the build leaves FSR 3 out, cmake/gfx.cmake).
//
// It generates a frame between two of the game's: the 3D picture only, the HUD is taken from the real frames. The pieces it needs are not the
// engine's own: a swapchain made by the FidelityFX API (it replaces the engine's, and presents the generated frames), four queues (the device
// makes them, gfx.h Device::framegen_queues) and the FidelityFX frame-generation DLL, which the game does not ship: the player puts it next to the
// loader the upscaler uses.
#pragma once
#include "swapchain_fns.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"

namespace nxrt::extras {

// The swapchain calls of the frame-generation swapchain, for the renderer to use in place of Vulkan's (swapchain_fns.h). Null when [framegen]
// enabled is off, the DLL or the queues are missing, or the first swapchain could not be made by AMD's code (that case falls back to a Vulkan
// swapchain: the table's functions forward to Vulkan from then on, and the game runs without frame generation). One per process.
const gfx::SwapchainFns* framegen_swapchain(gfx::Device& dev);

// True when the swapchain in use is AMD's and nothing has failed: the temporal pass (temporal.h) then runs for it every frame with a camera.
bool framegen_active();

// At the tone-map draw of a frame with a camera, from the temporal pass: records the prepare dispatch (depth, motion vectors, camera) into cb.
// TemporalFrame::out is the size of the tone-map's target, which must have the window's shape.
void framegen_prepare(VkCommandBuffer cb, const TemporalFrame& f);

// The presents since the swapchain was made, generated frames included (the frames shown on the screen); false: not frame generation, or the DLL
// does not count them. For the frame-rate counter, which shows these next to the frames the game renders.
bool framegen_present_count(uint64_t* count);

// After that draw, outside a render pass: copies the picture (src of size e, in src_layout, which is left as it was) before the HUD is drawn on it,
// converted to the swapchain's format and size. False: not copied (no prepare this frame, a shape that is not the window's).
bool framegen_hudless_capture(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkExtent2D e);

}  // namespace nxrt::extras
