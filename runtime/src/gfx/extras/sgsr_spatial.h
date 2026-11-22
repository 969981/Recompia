// SPDX-License-Identifier: GPL-3.0-or-later
// Cheap single-pass scene upscalers for the draw that enlarges the scene into the window texture ([gfx] scene_upscale): Snapdragon Game Super
// Resolution 1 (Qualcomm, BSD-3-Clause, edge-directed reconstruction with a built-in sharpen: one bilinear tap per pixel plus four gathers on
// edges only) or Catmull-Rom bicubic (9 bilinear taps). One compute pass each, any scale. The shaders are in sgsr_shaders.inc
// (scripts/gen_sgsr_shaders.py).
#pragma once
#include <volk.h>

#include <cstdint>

#include "nxrt/gfx.h"
#include "extras/spatial_upscaler.h"

namespace nxrt::extras {

class SgsrSpatial : public SpatialUpscaler {
public:
    ~SgsrSpatial() override { shutdown(); }
    SgsrSpatial(const SgsrSpatial&) = delete;
    SgsrSpatial& operator=(const SgsrSpatial&) = delete;

    enum class Kind { Sgsr, Bicubic };
    explicit SgsrSpatial(Kind k) : kind_(k) {}
    bool init(gfx::Device& dev);
    void shutdown();
    bool initialised() const { return dev_ != nullptr; }

    VkImageView upscale(VkCommandBuffer cb, VkImageView src, VkExtent2D in_view, VkExtent2D in_tex, VkExtent2D out, bool linear_hdr, float prescale,
                        float sharpness, VkExtent2D out_tex = {}) override;
    uint32_t frames() const { return frames_; }

    static constexpr uint32_t kSetRing = 16;

private:
    Kind kind_ = Kind::Sgsr;
    gfx::Device* dev_ = nullptr;
    VkSampler linear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kSetRing] = {};
    uint32_t next_ = 0;
    gfx::Image img_;
    VkExtent2D extent_ = {};
    bool img_init_ = false;
    uint32_t frames_ = 0;
};

}  // namespace nxrt::extras

namespace nxrt::extras {
// [gfx] scene_upscale: "sgsr", "bicubic" or empty (off, read once); the upscaler instance for it (created on first use; null when off or failed).
const char* scene_upscale_name();
SgsrSpatial* scene_cheap_upscaler(void* gfx_device);
}  // namespace nxrt::extras
