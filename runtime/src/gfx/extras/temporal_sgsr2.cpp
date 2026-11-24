// SPDX-License-Identifier: GPL-3.0-or-later
// Snapdragon Game Super Resolution 2 (Qualcomm, BSD-3-Clause, third_party/qualcomm_sgsr/v2) as a temporal backend (temporal.h): the
// 2-pass compute version, plain Vulkan compute, so it runs on every platform (desktop and phones). Shaders: scripts/gen_sgsr_shaders.py.
//   convert  render size: depth dilation and depth clip, the motion (the input pass's, previous minus current in UV units), colour to YCoCg
//   upscale  output size: 9-tap Lanczos of the jittered input, history reprojected by the motion and clamped to the neighbourhood, blend
// The history is two output-sized images, swapped each frame. Its own images and parameter ring live here; the inputs are the temporal pass's.
#include "extras/temporal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "sgsr2";

#include "extras/sgsr2_shaders.inc"

// The shaders' std140 block (gen_sgsr_shaders.py SGSR2_PARAMS).
struct Sgsr2Params {
    uint32_t render_size[2];
    uint32_t display_size[2];
    float render_rcp[2];
    float display_rcp[2];
    float jitter[2];
    float input_scale[2];          // the render size over the render-size images' size (they may be larger: a sub-rect)
    float clip_to_prev_clip[16];   // columns
    float pre_exposure;
    float fov_hor;                 // tan(horizontal fov / 2)
    float camera_near;
    float min_lerp;
    uint32_t same_camera;
    uint32_t reset;
    uint32_t depth_inverted;
};
static_assert(offsetof(Sgsr2Params, clip_to_prev_clip) == 48 && offsetof(Sgsr2Params, pre_exposure) == 112 && offsetof(Sgsr2Params, depth_inverted) == 136,
              "std140 layout of the SGSR 2 block");

constexpr uint32_t kRing = 8;          // frames that may be in flight with their own parameter slot and descriptor sets
constexpr VkDeviceSize kSlot = 256;    // a parameter slot (minUniformBufferOffsetAlignment is at most 256)

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss;
    b.srcAccessMask = sa;
    b.dstStageMask = ds;
    b.dstAccessMask = da;
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

class Sgsr2 final : public TemporalBackend {
public:
    const char* name() const override { return "SGSR 2"; }

    bool init(gfx::Device& dev) {
        dev_ = &dev;
        VkDevice d = dev.device();
        VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(d, &sci, nullptr, &nearest_) != VK_SUCCESS) return false;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(d, &sci, nullptr, &linear_) != VK_SUCCESS) return false;
        // both passes: binding 0 the block, 1..3 combined image samplers, 4..5 storage images
        VkDescriptorSetLayoutBinding b[6];
        for (uint32_t i = 0; i < 6; ++i)
            b[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 6;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout_;
        if (vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) return false;
        if (!pipeline(kSgsr2ConvertComp, sizeof kSgsr2ConvertComp, &convert_) || !pipeline(kSgsr2UpscaleComp, sizeof kSgsr2UpscaleComp, &upscale_)) return false;
        const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kRing}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * kRing},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kRing}};
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 2 * kRing;
        pci.poolSizeCount = 3;
        pci.pPoolSizes = ps;
        if (vkCreateDescriptorPool(d, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
        VkDescriptorSetLayout ls[2 * kRing];
        std::fill(std::begin(ls), std::end(ls), set_layout_);
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 2 * kRing;
        ai.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(d, &ai, sets_) != VK_SUCCESS) return false;
        if (dev.create_buffer(kSlot * kRing, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, gfx::MemoryUse::Upload, &params_, "sgsr2 params") != VK_SUCCESS ||
            !params_.mapped)
            return false;
        // [prof] sgsr2_ms: GPU timestamps around the two passes, the mean logged every 300 frames (for benchmarks; off: no queries)
        if (config_get_bool("prof", "sgsr2_ms", false) && dev.timestamps_supported()) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 * kRing;
            if (vkCreateQueryPool(d, &qi, nullptr, &qpool_) != VK_SUCCESS) qpool_ = VK_NULL_HANDLE;
        }
        NXLOG_I(kTag, "Snapdragon Game Super Resolution 2 ready (2-pass compute%s)", qpool_ ? ", GPU time logged" : "");
        return true;
    }

    bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) override {
        const VkExtent2D in_ext{f.color.width, f.color.height};   // the inputs' size, at least the render size: the same sub-rect here
        if (!image(mda_, in_ext, VK_FORMAT_R16G16B16A16_SFLOAT, "sgsr2 motion depth clip") || !image(ycocg_, in_ext, VK_FORMAT_R32_UINT, "sgsr2 ycocg") ||
            !image(hist_[0], f.out, VK_FORMAT_R16G16B16A16_SFLOAT, "sgsr2 history 0") || !image(hist_[1], f.out, VK_FORMAT_R16G16B16A16_SFLOAT, "sgsr2 history 1"))
            return false;
        const uint32_t slot = next_;
        next_ = (next_ + 1) % kRing;
        if (qpool_) {   // this slot's pair from kRing frames ago is done by now (or not ready: skipped)
            uint64_t ts[2] = {};
            if (stamped_[slot] && vkGetQueryPoolResults(dev_->device(), qpool_, slot * 2, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                gpu_ms_ += static_cast<double>(ts[1] - ts[0]) * dev_->properties().limits.timestampPeriod * 1e-6;
                if (++gpu_n_ == 300) {
                    NXLOG_I(kTag, "GPU time of the two passes: %.3f ms per frame (mean of 300, %ux%u -> %ux%u)", gpu_ms_ / gpu_n_, f.render.width, f.render.height,
                            f.out.width, f.out.height);
                    gpu_ms_ = 0;
                    gpu_n_ = 0;
                }
            }
            vkCmdResetQueryPool(cb, qpool_, slot * 2, 2);
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool_, slot * 2);
            stamped_[slot] = true;
        }
        const uint32_t cur = frame_ & 1, prev = cur ^ 1;
        ++frame_;

        // the block
        Sgsr2Params p = {};
        p.render_size[0] = f.render.width;
        p.render_size[1] = f.render.height;
        p.display_size[0] = f.out.width;
        p.display_size[1] = f.out.height;
        p.render_rcp[0] = 1.0f / static_cast<float>(f.render.width);
        p.render_rcp[1] = 1.0f / static_cast<float>(f.render.height);
        p.display_rcp[0] = 1.0f / static_cast<float>(f.out.width);
        p.display_rcp[1] = 1.0f / static_cast<float>(f.out.height);
        p.jitter[0] = f.jitter_px[0];   // the FSR convention (temporal.h) is SGSR 2's: the picture's offset in render pixels, +y down
        p.jitter[1] = f.jitter_px[1];
        p.input_scale[0] = static_cast<float>(f.render.width) / static_cast<float>(in_ext.width);
        p.input_scale[1] = static_cast<float>(f.render.height) / static_cast<float>(in_ext.height);
        std::memcpy(p.clip_to_prev_clip, f.clip_to_prev_clip, sizeof p.clip_to_prev_clip);
        p.pre_exposure = 1.0f;
        p.fov_hor = std::tan(f.fov_y * 0.5f) * f.aspect;
        p.camera_near = f.near_z;
        // a still camera (README: the view-projections differ by less than 1e-5 in all): clip_to_prev_clip is then the identity
        float diff = 0;
        for (int i = 0; i < 16; ++i) diff += std::fabs(f.clip_to_prev_clip[i] - ((i % 5) == 0 ? 1.0f : 0.0f));
        const bool still = !f.reset && diff < 1e-5f;
        p.same_camera = still ? 1u : 0u;
        p.min_lerp = still ? 0.3f : 0.0f;   // ponytail: the vendor sample's value for a still camera; tune on device if the still picture swims
        p.reset = f.reset ? 1u : 0u;
        p.depth_inverted = f.depth_inverted ? 1u : 0u;
        std::memcpy(static_cast<uint8_t*>(params_.mapped) + slot * kSlot, &p, sizeof p);
        dev_->flush_buffer(params_);

        // descriptors: convert reads colour, depth, motion and writes mda + ycocg; upscale reads history[prev], mda, ycocg and writes output + history[cur]
        const VkDescriptorBufferInfo bi = {params_.buffer, slot * kSlot, sizeof p};
        auto write = [&](VkDescriptorSet set, const VkDescriptorImageInfo (&ii)[5]) {
            VkWriteDescriptorSet w[6] = {};
            for (uint32_t i = 0; i < 6; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = set;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (i == 0) w[i].pBufferInfo = &bi;
                else w[i].pImageInfo = &ii[i - 1];
            }
            vkUpdateDescriptorSets(dev_->device(), 6, w, 0, nullptr);
        };
        const VkImageLayout g = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorSet sc = sets_[slot * 2], su = sets_[slot * 2 + 1];
        write(sc, {{nearest_, f.color.view, g}, {nearest_, f.depth.view, g}, {nearest_, f.motion.view, g}, {VK_NULL_HANDLE, mda_.img.view, g},
                   {VK_NULL_HANDLE, ycocg_.img.view, g}});
        write(su, {{linear_, hist_[prev].img.view, g}, {nearest_, mda_.img.view, g}, {nearest_, ycocg_.img.view, g}, {VK_NULL_HANDLE, f.output.view, g},
                   {VK_NULL_HANDLE, hist_[cur].img.view, g}});

        const VkPipelineStageFlags2 any = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const VkAccessFlags2 rw = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        for (Target* t : {&mda_, &ycocg_, &hist_[0], &hist_[1]}) {
            barrier(cb, t->img.image, t->ready ? g : VK_IMAGE_LAYOUT_UNDEFINED, any, rw, any, rw);
            if (!t->ready) {   // a new image holds no NaN: SGSR 2 blends the history with a weight of 0 on a reset, and 0 * NaN is NaN
                const VkClearColorValue zero{};
                const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cb, t->img.image, g, &zero, 1, &all);
                barrier(cb, t->img.image, g, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, cs, rw);
                t->ready = true;
            }
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, convert_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sc, 0, nullptr);
        vkCmdDispatch(cb, (f.render.width + 7) / 8, (f.render.height + 7) / 8, 1);
        for (Target* t : {&mda_, &ycocg_}) barrier(cb, t->img.image, g, cs, VK_ACCESS_2_SHADER_WRITE_BIT, cs, VK_ACCESS_2_SHADER_READ_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &su, 0, nullptr);
        vkCmdDispatch(cb, (f.out.width + 7) / 8, (f.out.height + 7) / 8, 1);
        if (qpool_) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, slot * 2 + 1);
        if (frame_ == 1)
            NXLOG_I(kTag, "first frame: %ux%u -> %ux%u, jitter %+.3f %+.3f px, %s depth, horizontal fov term %.3f", f.render.width, f.render.height, f.out.width,
                    f.out.height, static_cast<double>(f.jitter_px[0]), static_cast<double>(f.jitter_px[1]), f.depth_inverted ? "reversed" : "forward",
                    static_cast<double>(p.fov_hor));
        return true;
    }

private:
    struct Target {
        gfx::Image img;
        bool ready = false;
    };

    bool pipeline(const uint32_t* code, size_t bytes, VkPipeline* out) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes;
        mi.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev_->device(), &mi, nullptr, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = layout_;
        const VkResult r = vkCreateComputePipelines(dev_->device(), VK_NULL_HANDLE, 1, &ci, nullptr, out);
        vkDestroyShaderModule(dev_->device(), m, nullptr);
        return r == VK_SUCCESS;
    }

    // (Re)makes an image of that size and format (sizes change rarely: the GPU is waited for).
    bool image(Target& t, VkExtent2D e, VkFormat fmt, const char* name) {
        if (t.img && t.img.extent.width == e.width && t.img.extent.height == e.height) return true;
        if (t.img) {
            dev_->wait_idle();
            dev_->destroy_image(t.img);
        }
        gfx::ImageDesc d;
        d.format = fmt;
        d.width = e.width;
        d.height = e.height;
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.name = name;
        std::string why;
        t.ready = false;
        if (dev_->create_image(d, &t.img, &why) != VK_SUCCESS) {
            NXLOG_E(kTag, "%s %ux%u: %s", name, e.width, e.height, why.c_str());
            t.img = {};
            return false;
        }
        return true;
    }

    gfx::Device* dev_ = nullptr;
    VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline convert_ = VK_NULL_HANDLE, upscale_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2 * kRing] = {};
    gfx::Buffer params_;
    uint32_t next_ = 0;
    uint64_t frame_ = 0;
    VkQueryPool qpool_ = VK_NULL_HANDLE;
    bool stamped_[kRing] = {};
    double gpu_ms_ = 0;
    uint32_t gpu_n_ = 0;
    Target mda_, ycocg_, hist_[2];
};

}  // namespace

TemporalBackend* temporal_sgsr2(void* gfx_device) {
    static Sgsr2* inst = nullptr;
    static bool tried = false;
    if (!tried && gfx_device) {
        tried = true;
        auto* s = new Sgsr2();
        if (s->init(*static_cast<gfx::Device*>(gfx_device))) inst = s;
        else {
            NXLOG_E(kTag, "could not be set up: temporal upscaling off");
            delete s;   // ponytail: the Vulkan objects made before the failure are leaked (once per run, never in practice)
        }
    }
    return inst;
}

}  // namespace nxrt::extras
