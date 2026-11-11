// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 frame generation, the frames (framegen.h; the swapchain is framegen_ffx.cpp). Each frame with a camera:
//   at the tone-map draw  the prepare dispatch (depth, motion vectors, camera: from the temporal pass, temporal.h) and, after the draw, a copy of
//                         the picture before the HUD is drawn on it (the "HUD-less" colour)
//   at the present        the context is configured with that copy; AMD's swapchain asks back for the generated frame (frameGenerationCallback)
//                         and for the frame to present (presentCallback), and takes the HUD from the difference between the rendered frame and
//                         the HUD-less copy, so the HUD is not interpolated.
// A frame without a camera (menus, loading) or without a HUD-less copy is presented as it is.
#include "extras/framegen.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "extras/ffx_resource.h"
#include "extras/framegen_internal.h"
#include "extras/temporal.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "framegen";
fg::State& g = fg::state();

VkImageLayout layout_of(uint32_t state) {
    if (state & FFX_API_RESOURCE_STATE_PRESENT) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (state & FFX_API_RESOURCE_STATE_COPY_DEST) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (state & FFX_API_RESOURCE_STATE_COPY_SRC) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (state & (FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COMPUTE_READ)) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// The frame to present: the frame generation swapchain hands over the rendered or generated picture and asks for it in the swapchain's image
// (nothing is composed on top: the HUD is already in it).
ffxReturnCode_t present_callback(ffxCallbackDescFrameGenerationPresent* p, void*) {
    const VkCommandBuffer cb = static_cast<VkCommandBuffer>(p->commandList);
    const VkImage src = reinterpret_cast<VkImage>(p->currentBackBuffer.resource), dst = reinterpret_cast<VkImage>(p->outputSwapChainBuffer.resource);
    if (!cb || !src || !dst || src == dst) return FFX_API_RETURN_OK;
    const VkImageLayout sl = layout_of(p->currentBackBuffer.state), dl = layout_of(p->outputSwapChainBuffer.state);
    barrier(cb, src, sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {std::min(p->currentBackBuffer.description.width, p->outputSwapChainBuffer.description.width),
                std::min(p->currentBackBuffer.description.height, p->outputSwapChainBuffer.description.height), 1};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dl);
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t generation_callback(ffxDispatchDescFrameGeneration* p, void*) { return g.dispatch(&g.ctx, &p->header); }

void destroy_context() {
    if (!g.ctx) return;
    g.dev->wait_idle();
    g.destroy_ctx(&g.ctx, nullptr);
    g.ctx = nullptr;
}

bool make_context(VkExtent2D render, bool inverted) {
    if (ffx_surface_format(g.format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        NXLOG_E(kTag, "the swapchain's format %d has no name in FidelityFX: frames are presented as they are", static_cast<int>(g.format));
        return false;
    }
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext.vkDevice = g.dev->device();
    be.deviceContext.vkPhysicalDevice = g.dev->physical();
    be.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescFrameGenerationVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = &ver.header;
    cd.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | (inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0);
    cd.displaySize = {g.extent.width, g.extent.height};
    cd.maxRenderSize = {render.width, render.height};
    cd.backBufferFormat = ffx_surface_format(g.format);
    const ffxReturnCode_t r = g.create_ctx(&g.ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        NXLOG_E(kTag, "ffxCreateContext (frame generation %ux%u) failed (%u): frames are presented as they are", g.extent.width, g.extent.height, r);
        g.ctx = nullptr;
        return false;
    }
    g.render = render;
    g.depth_inverted = inverted;
    NXLOG_I(kTag, "frame generation context made: display %ux%u, render %ux%u, %s depth", g.extent.width, g.extent.height, render.width, render.height,
            inverted ? "reversed" : "forward");
    return true;
}

// The picture and the window have the same shape (the present pass scales one into the other without bars): within 1 % of the width.
bool same_shape(VkExtent2D e) {
    const int64_t d = static_cast<int64_t>(e.width) * g.extent.height - static_cast<int64_t>(e.height) * g.extent.width;
    return std::llabs(d) * 100 <= static_cast<int64_t>(e.width) * g.extent.height;
}

}  // namespace

bool framegen_active() { return g.live && !g.native && !g.failed; }

void framegen_prepare(VkCommandBuffer cb, const TemporalFrame& f) {
    if (!framegen_active()) return;
    // the HUD is taken from the difference to the HUD-less copy, which is made at the window's size: the picture must have the window's shape
    // (a letterboxed picture, [display] keep_aspect on a window that is not 16:9, would need the bars as well)
    if (!same_shape(f.out)) {
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_W(kTag, "the game's picture is %ux%u and the window %ux%u: not the same shape, frames are presented as they are", f.out.width, f.out.height,
                    g.extent.width, g.extent.height);
        }
        return;
    }
    if (g.ctx && (f.render.width > g.render.width || f.render.height > g.render.height || f.depth_inverted != g.depth_inverted)) destroy_context();
    if (!g.ctx) {
        if (!make_context(f.render, f.depth_inverted)) {
            g.failed = true;
            return;
        }
    }
    ffxDispatchDescFrameGenerationPrepareV2 d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = g.frame_id + 1;
    d.commandList = cb;
    d.renderSize = {f.render.width, f.render.height};
    d.jitterOffset = {f.jitter_px[0], f.jitter_px[1]};
    d.motionVectorScale = {f.mv_scale[0], f.mv_scale[1]};
    d.frameTimeDelta = f.frame_ms;
    d.reset = f.reset;
    d.cameraNear = f.near_z;
    d.cameraFar = f.far_z;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.depth = ffx_resource(f.depth.image, f.depth.format, f.depth.width, f.depth.height, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.motionVectors = ffx_resource(f.motion.image, f.motion.format, f.motion.width, f.motion.height, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
                                   FFX_API_RESOURCE_USAGE_UAV);
    for (int i = 0; i < 3; ++i) {
        d.cameraPosition[i] = f.cam_pos[i];
        d.cameraUp[i] = f.cam_up[i];
        d.cameraRight[i] = f.cam_right[i];
        d.cameraForward[i] = f.cam_fwd[i];
    }
    const ffxReturnCode_t r = g.dispatch(&g.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        NXLOG_E(kTag, "ffxDispatch (frame generation prepare) failed (%u): frames are presented as they are", r);
        g.failed = true;
        return;
    }
    g.prepared = true;
}

bool framegen_hudless_capture(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkExtent2D e) {
    if (!framegen_active() || !g.prepared || !same_shape(e)) return false;
    const uint32_t slot = g.hudless_next;
    gfx::Image& h = g.hudless[slot];
    if (!h || h.extent.width != g.extent.width || h.extent.height != g.extent.height) {
        g.dev->wait_idle();
        g.dev->destroy_image(h);
        gfx::ImageDesc id;
        id.format = g.format;                  // the swapchain's: the blit converts to it, as the present pass does
        id.width = g.extent.width;
        id.height = g.extent.height;
        id.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        id.create_view = false;
        id.name = "frame generation hudless";
        std::string why;
        if (g.dev->create_image(id, &h, &why) != VK_SUCCESS) {
            NXLOG_E(kTag, "the HUD-less image could not be made (%s): frames are presented as they are", why.c_str());
            g.failed = true;
            return false;
        }
    }
    barrier(cb, src, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, h.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {};      // scaled to the window's size and made the swapchain's format, as the present pass will do to the whole picture
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {static_cast<int32_t>(e.width), static_cast<int32_t>(e.height), 1};
    bl.dstOffsets[1] = {static_cast<int32_t>(g.extent.width), static_cast<int32_t>(g.extent.height), 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, h.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout);
    barrier(cb, h.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g.hudless_current = static_cast<int>(slot);
    g.hudless_next = (slot + 1) % fg::kHudlessRing;
    return true;
}

namespace fg {

void frame_before_present() {
    ++g.frame_id;
    if (!g.ctx || g.failed) return;
    const bool generate = g.prepared && g.hudless_current >= 0;
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g.live;
    c.presentCallback = present_callback;
    c.frameGenerationCallback = generation_callback;
    c.frameGenerationEnabled = generate;
    c.allowAsyncWorkloads = true;
    if (generate) {
        const gfx::Image& h = g.hudless[g.hudless_current];
        c.HUDLessColor = ffx_resource(h.image, h.format, h.extent.width, h.extent.height, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
    }
    c.generationRect = {0, 0, static_cast<int32_t>(g.extent.width), static_cast<int32_t>(g.extent.height)};
    c.frameID = g.frame_id;
    const ffxReturnCode_t r = g.configure(&g.ctx, &c.header);
    if (r != FFX_API_RETURN_OK) {
        NXLOG_E(kTag, "ffxConfigure (frame generation) failed (%u): frames are presented as they are", r);
        g.failed = true;
    }
    static uint64_t n = 0;
    if (generate && ++n == 1) NXLOG_I(kTag, "frame generation on: the first frame configured with its HUD-less copy");
    g.prepared = false;
    g.hudless_current = -1;
}

void frame_destroy() {
    destroy_context();
    for (gfx::Image& h : g.hudless) g.dev->destroy_image(h);
    g.prepared = false;
    g.hudless_current = -1;
}

}  // namespace fg
}  // namespace nxrt::extras
