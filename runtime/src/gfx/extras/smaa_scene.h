// SPDX-License-Identifier: GPL-3.0-or-later
// SMAA (third_party/smaa, MIT) on the game's HDR scene, before its tone map: edge detection, blend weights and neighbourhood blending as three
// full-screen graphics passes into an RGBA16F image the size of the scene. Only the 3D scene goes through it (the tone-map draw reads the result
// instead of the scene), so the HUD and the menus drawn after the tone map are not filtered. The shaders are in smaa_shaders.inc
// (scripts/gen_smaa_shaders.py); the area and search lookup tables come from AreaTex.h / SearchTex.h.
#pragma once
#include <volk.h>

#include <cstdint>

#include "nxrt/extras.h"
#include "nxrt/gfx.h"

namespace nxrt::extras {

class SmaaScene {
public:
    SmaaScene() = default;
    ~SmaaScene() { shutdown(); }
    SmaaScene(const SmaaScene&) = delete;
    SmaaScene& operator=(const SmaaScene&) = delete;

    bool init(gfx::Device& dev);
    void shutdown();
    bool initialised() const { return dev_ != nullptr; }

    // Records into cb (outside any render pass). src: a view of the scene image in SHADER_READ_ONLY_OPTIMAL, extent its size (the whole image is
    // the scene). Returns a view of the anti-aliased scene (GENERAL layout, readable by fragment and compute shaders after the recorded barrier),
    // or null when nothing was recorded (mode Off, or a pass could not be made).
    VkImageView apply(VkCommandBuffer cb, VkImageView src, VkExtent2D extent, AaMode mode);
    uint32_t frames() const { return frames_; }

    static constexpr uint32_t kSetRing = 64;

private:
    struct Targets {
        gfx::Image edges, weights, out;
        VkExtent2D extent = {};
    };
    bool ensure_targets(VkExtent2D e);
    void destroy_targets();
    // One SMAA run over `in` (a view in SHADER_READ_ONLY_OPTIMAL) at a quality level (0 low, 1 medium, 2 high, 3 ultra); returns the result's view.
    VkImageView run(VkCommandBuffer cb, VkImageView in, VkExtent2D e, int level);
    VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkFormat fmt);

    gfx::Device* dev_ = nullptr;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kSetRing] = {};
    uint32_t next_ = 0;
    VkShaderModule mods_[12] = {};
    VkPipeline edge_[4] = {}, weight_[4] = {}, blend_ = VK_NULL_HANDLE;
    gfx::Image area_, search_;
    Targets t_;
    uint32_t frames_ = 0;
};

}  // namespace nxrt::extras
