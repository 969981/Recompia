// SPDX-License-Identifier: GPL-3.0-or-later
// A phone's scene upscale writes the game's stretch target itself (nvn_gpu_internal.h tex_blit_from; nvn_replay_upscale.cpp).
#include "nvn_gpu_texture_internal.h"

namespace nxrt::nvn::be::gpu {
using namespace tex;

bool tex_blit_from(Texture* t, Frame& f, VkImage src, VkFormat src_fmt, VkExtent2D ext) {
    TexGpu* g = tex_or_null(t);
    if (!g || g->depth || !src || !(g->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
    const auto has = [](VkFormat fm, VkFormatFeatureFlags need) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(device().physical(), fm, &p);
        return (p.optimalTilingFeatures & need) == need;
    };
    static VkFormat ok_src = VK_FORMAT_UNDEFINED, ok_dst = VK_FORMAT_UNDEFINED;   // the pair last found to work (one call a frame)
    if (src_fmt != ok_src || g->format != ok_dst) {
        if (!has(src_fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !has(g->format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            static bool told = false;
            if (!told) {
                told = true;
                NXLOG_W(kTag, "scene upscale: format %d cannot be blitted into format %d: the game's own stretch draws", static_cast<int>(src_fmt),
                        static_cast<int>(g->format));
            }
            return false;
        }
        ok_src = src_fmt;
        ok_dst = g->format;
    }
    move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0);
    const int32_t w = static_cast<int32_t>(ext.width), h = static_cast<int32_t>(ext.height);
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {w, h, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {w, h, 1};
    vkCmdBlitImage(f.cb, src, VK_IMAGE_LAYOUT_GENERAL, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    return true;
}

}  // namespace nxrt::nvn::be::gpu
