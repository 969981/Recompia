// SPDX-License-Identifier: GPL-3.0-or-later
// Single-pass scene upscalers (sgsr_spatial.h).
#include "extras/sgsr_spatial.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>

#include "nxrt/extras.h"
#include "nxrt/log.h"
#include "nxrt/config.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "scene_upscale";
constexpr uint32_t kBlock = 8;                    // local size of both shaders
constexpr uint32_t kPushBytes = 48;               // vec4 vp (1 / input size, input size), vec4 out (size, uv step), vec4 clip (largest uv)

#include "extras/sgsr_shaders.inc"

void to_general(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}
}  // namespace

bool SgsrSpatial::init(gfx::Device& dev) {
    if (dev_) return true;
    dev_ = &dev;
    VkDevice d = dev.device();
    auto fail = [&](const char* what) {
        NXLOG_E(kTag, "%s failed", what);
        shutdown();
        return false;
    };
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d, &sci, nullptr, &linear_) != VK_SUCCESS) return fail("sampler");

    const VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 3;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &set_layout_) != VK_SUCCESS) return fail("set layout");
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) return fail("pipeline layout");

    const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLER, kSetRing}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetRing}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetRing}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSetRing;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return fail("descriptor pool");
    VkDescriptorSetLayout ls[kSetRing];
    for (auto& x : ls) x = set_layout_;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSetRing;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(d, &ai, sets_) != VK_SUCCESS) return fail("descriptor sets");

    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = kind_ == Kind::Sgsr ? sizeof kSgsrComp : sizeof kBicubicComp;
    mi.pCode = kind_ == Kind::Sgsr ? kSgsrComp : kBicubicComp;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return fail("shader module");
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = layout_;
    const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe_);
    vkDestroyShaderModule(d, m, nullptr);
    if (r != VK_SUCCESS) return fail("pipeline");
    NXLOG_I(kTag, "%s ready", kind_ == Kind::Sgsr ? "Snapdragon Game Super Resolution 1 (edge direction)" : "Catmull-Rom bicubic");
    return true;
}

void SgsrSpatial::shutdown() {
    if (!dev_) return;
    VkDevice d = dev_->device();
    dev_->wait_idle();
    if (img_) dev_->destroy_image(img_);
    img_ = gfx::Image();
    extent_ = {};
    img_init_ = false;
    if (pipe_) vkDestroyPipeline(d, pipe_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (linear_) vkDestroySampler(d, linear_, nullptr);
    pipe_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    linear_ = VK_NULL_HANDLE;
    std::fill(std::begin(sets_), std::end(sets_), VK_NULL_HANDLE);
    next_ = 0;
    dev_ = nullptr;
}

VkImageView SgsrSpatial::upscale(VkCommandBuffer cb, VkImageView src, VkExtent2D in_view, VkExtent2D in_tex, VkExtent2D out, bool linear_hdr, float prescale,
                                float sharpness, VkExtent2D out_tex) {
    (void)linear_hdr;
    (void)prescale;
    (void)sharpness;   // SGSR sharpens by its own constant (EdgeSharpness 2); Catmull-Rom has none
    if (!dev_ || !src || !in_view.width || !in_view.height || !out.width || !out.height) return VK_NULL_HANDLE;
    const VkExtent2D res{std::max(out.width, out_tex.width), std::max(out.height, out_tex.height)};   // the image; the pass writes the out part only
    // the input part (from texel 0, 0) fills the output: uv over out maps to that part of the whole input image
    const float push[12] = {1.0f / static_cast<float>(in_tex.width), 1.0f / static_cast<float>(in_tex.height), static_cast<float>(in_tex.width),
                           static_cast<float>(in_tex.height), static_cast<float>(out.width), static_cast<float>(out.height),
                           static_cast<float>(in_view.width) / static_cast<float>(in_tex.width) / static_cast<float>(out.width),
                           static_cast<float>(in_view.height) / static_cast<float>(in_tex.height) / static_cast<float>(out.height),
                            // the centre stays 2 texels inside the part: SGSR's gathers and Catmull-Rom reach 2 texels past it, and outside the
                            // part a scene of larger targets holds stale pixels (the game's dynamic resolution)
                            std::max(0.5f, static_cast<float>(in_view.width) - 2.0f) / static_cast<float>(in_tex.width),
                            std::max(0.5f, static_cast<float>(in_view.height) - 2.0f) / static_cast<float>(in_tex.height), 0.0f, 0.0f};
    if (!img_ || extent_.width != res.width || extent_.height != res.height) {
        if (img_) {
            dev_->wait_idle();
            dev_->destroy_image(img_);
            img_ = gfx::Image();
        }
        gfx::ImageDesc id;
        id.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        id.width = res.width;
        id.height = res.height;
        id.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        id.name = kind_ == Kind::Sgsr ? "sgsr out" : "bicubic out";
        std::string why;
        if (dev_->create_image(id, &img_, &why) != VK_SUCCESS) {
            NXLOG_E(kTag, "output image: %s", why.c_str());
            img_ = gfx::Image();
            return VK_NULL_HANDLE;
        }
        extent_ = res;
        img_init_ = false;
    }
    const bool fresh = !img_init_;
    to_general(cb, img_.image, img_init_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_WRITE_BIT);
    img_init_ = true;
    if (fresh && (res.width > out.width || res.height > out.height)) {   // a result image larger than out: zero it once (the draw that samples it may touch the texel past out; no NaN from undefined memory)
        const VkClearColorValue zero{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, img_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        to_general(cb, img_.image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    VkDescriptorSet set = sets_[next_];
    next_ = (next_ + 1) % kSetRing;
    const VkDescriptorImageInfo smp = {linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo in = {VK_NULL_HANDLE, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo dst = {VK_NULL_HANDLE, img_.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    const VkDescriptorImageInfo* infos[3] = {&smp, &in, &dst};
    VkWriteDescriptorSet w[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = types[i];
        w[i].pImageInfo = infos[i];
    }
    vkUpdateDescriptorSets(dev_->device(), 3, w, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes, push);
    vkCmdDispatch(cb, (out.width + kBlock - 1) / kBlock, (out.height + kBlock - 1) / kBlock, 1);
    to_general(cb, img_.image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    ++frames_;
    return img_.view;
}

std::atomic<int> g_row_up{-1};
void set_scene_upscale_live(int kind) { g_row_up.store(kind, std::memory_order_relaxed); }

const char* scene_upscale_name() {
    if (const int w = scene_upscale_watch(); w >= 0) return w == 1 ? "sgsr" : w == 2 ? "bicubic" : "";   // developer key, live
    if (const int r = g_row_up.load(std::memory_order_relaxed); r >= 0) return r == 1 ? "sgsr" : r == 2 ? "bicubic" : "";
    static const std::string v = [] {
        std::string s = config_get_str("gfx", "scene_upscale", "");
        if (s != "sgsr" && s != "bicubic") s.clear();
        return s;
    }();
    return v.c_str();
}

SgsrSpatial* scene_cheap_upscaler(void* gfx_device) {
    static SgsrSpatial* up[2] = {};   // one per kind, made on first use (a live switch back costs nothing)
    static bool tried[2] = {};
    if (!gfx_device || !*scene_upscale_name()) return nullptr;
    const int k = std::string(scene_upscale_name()) == "sgsr" ? 0 : 1;
    if (!tried[k]) {
        tried[k] = true;
        auto* u = new SgsrSpatial(k == 0 ? SgsrSpatial::Kind::Sgsr : SgsrSpatial::Kind::Bicubic);
        if (u->init(*static_cast<gfx::Device*>(gfx_device))) up[k] = u;
        else delete u;
    }
    return up[k];
}

}  // namespace nxrt::extras
