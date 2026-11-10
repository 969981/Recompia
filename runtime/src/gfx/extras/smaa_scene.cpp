// SPDX-License-Identifier: GPL-3.0-or-later
// SMAA on the HDR scene (smaa_scene.h).
#include "extras/smaa_scene.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

#include "nxrt/log.h"
#include "../../../third_party/smaa/AreaTex.h"
#include "../../../third_party/smaa/SearchTex.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "smaa";

#include "extras/smaa_shaders.inc"

enum Mod { kEdgeVert, kWeightVert, kBlendVert, kEdgeLow, kEdgeMed, kEdgeHigh, kEdgeUltra, kWeightLow, kWeightMed, kWeightHigh, kWeightUltra, kBlendFrag, kModCount };

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

// Uploads a tightly packed 2D texture into a new sampled image (one-time, waits for the copy).
bool upload_texture(gfx::Device& dev, VkFormat fmt, uint32_t w, uint32_t h, const void* data, size_t bytes, const char* name, gfx::Image* out) {
    gfx::ImageDesc id;
    id.format = fmt;
    id.width = w;
    id.height = h;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.name = name;
    std::string why;
    if (dev.create_image(id, out, &why) != VK_SUCCESS) {
        NXLOG_E(kTag, "%s image: %s", name, why.c_str());
        return false;
    }
    gfx::Buffer buf;
    if (dev.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryUse::Upload, &buf, name) != VK_SUCCESS || !buf.mapped) {
        NXLOG_E(kTag, "%s staging buffer failed", name);
        dev.destroy_image(*out);
        *out = gfx::Image();
        return false;
    }
    std::memcpy(buf.mapped, data, bytes);
    dev.flush_buffer(buf);
    const bool ok = dev.submit_once([&](VkCommandBuffer cb) {
        barrier(cb, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, buf.buffer, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier(cb, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, kShaderStages, VK_ACCESS_2_SHADER_READ_BIT);
    });
    dev.destroy_buffer(buf);
    if (!ok) {
        NXLOG_E(kTag, "%s upload failed", name);
        dev.destroy_image(*out);
        *out = gfx::Image();
    }
    return ok;
}
}  // namespace

VkPipeline SmaaScene::make_pipeline(VkShaderModule vs, VkShaderModule fs, VkFormat fmt) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pri.colorAttachmentCount = 1;
    pri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_->device(), VK_NULL_HANDLE, 1, &gpi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

bool SmaaScene::init(gfx::Device& dev) {
    if (dev_) return true;
    dev_ = &dev;
    VkDevice d = dev.device();
    auto fail = [&](const char* what) {
        NXLOG_E(kTag, "%s failed", what);
        shutdown();
        return false;
    };
    auto sampler = [&](VkFilter f, VkSampler* out) {
        VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = f;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        return vkCreateSampler(d, &sci, nullptr, out) == VK_SUCCESS;
    };
    if (!sampler(VK_FILTER_LINEAR, &linear_) || !sampler(VK_FILTER_NEAREST, &nearest_)) return fail("samplers");

    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 3;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &set_layout_) != VK_SUCCESS) return fail("set layout");
    const VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) return fail("pipeline layout");
    const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetRing * 3};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSetRing;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return fail("descriptor pool");
    VkDescriptorSetLayout ls[kSetRing];
    for (auto& x : ls) x = set_layout_;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSetRing;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(d, &ai, sets_) != VK_SUCCESS) return fail("descriptor sets");

    const struct { const uint32_t* code; size_t bytes; } src[kModCount] = {
        {kSmaaEdgeVert, sizeof kSmaaEdgeVert},       {kSmaaWeightVert, sizeof kSmaaWeightVert},   {kSmaaBlendVert, sizeof kSmaaBlendVert},
        {kSmaaEdgeLow, sizeof kSmaaEdgeLow},         {kSmaaEdgeMedium, sizeof kSmaaEdgeMedium},     {kSmaaEdgeHigh, sizeof kSmaaEdgeHigh},
        {kSmaaEdgeUltra, sizeof kSmaaEdgeUltra},     {kSmaaWeightLow, sizeof kSmaaWeightLow},       {kSmaaWeightMedium, sizeof kSmaaWeightMedium},
        {kSmaaWeightHigh, sizeof kSmaaWeightHigh},   {kSmaaWeightUltra, sizeof kSmaaWeightUltra},   {kSmaaBlend, sizeof kSmaaBlend}};
    for (int i = 0; i < kModCount; ++i) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = src[i].bytes;
        mi.pCode = src[i].code;
        if (vkCreateShaderModule(d, &mi, nullptr, &mods_[i]) != VK_SUCCESS) return fail("shader module");
    }
    for (int i = 0; i < 4; ++i) {
        edge_[i] = make_pipeline(mods_[kEdgeVert], mods_[kEdgeLow + i], VK_FORMAT_R8G8_UNORM);
        weight_[i] = make_pipeline(mods_[kWeightVert], mods_[kWeightLow + i], VK_FORMAT_R8G8B8A8_UNORM);
        if (!edge_[i] || !weight_[i]) return fail("pipelines");
    }
    blend_ = make_pipeline(mods_[kBlendVert], mods_[kBlendFrag], VK_FORMAT_R16G16B16A16_SFLOAT);
    if (!blend_) return fail("pipelines");

    if (!upload_texture(dev, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, areaTexBytes, AREATEX_SIZE, "smaa area", &area_)) return fail("area texture");
    if (!upload_texture(dev, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, searchTexBytes, SEARCHTEX_SIZE, "smaa search", &search_)) return fail("search texture");
    NXLOG_I(kTag, "SMAA ready (scene anti-aliasing: edge detection, blend weights, neighbourhood blend)");
    return true;
}

void SmaaScene::destroy_targets() {
    if (!dev_) return;
    for (gfx::Image* i : {&t_.edges, &t_.weights, &t_.out}) {
        if (*i) dev_->destroy_image(*i);
        *i = gfx::Image();
    }
    t_.extent = {};
}

void SmaaScene::shutdown() {
    if (!dev_) return;
    VkDevice d = dev_->device();
    dev_->wait_idle();
    destroy_targets();
    for (gfx::Image* i : {&area_, &search_}) {
        if (*i) dev_->destroy_image(*i);
        *i = gfx::Image();
    }
    for (VkPipeline* p : {&edge_[0], &edge_[1], &edge_[2], &edge_[3], &weight_[0], &weight_[1], &weight_[2], &weight_[3], &blend_}) {
        if (*p) vkDestroyPipeline(d, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkShaderModule& m : mods_) {
        if (m) vkDestroyShaderModule(d, m, nullptr);
        m = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (linear_) vkDestroySampler(d, linear_, nullptr);
    if (nearest_) vkDestroySampler(d, nearest_, nullptr);
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    linear_ = nearest_ = VK_NULL_HANDLE;
    std::fill(std::begin(sets_), std::end(sets_), VK_NULL_HANDLE);
    next_ = 0;
    dev_ = nullptr;
}

bool SmaaScene::ensure_targets(VkExtent2D e) {
    if (t_.edges && t_.extent.width == e.width && t_.extent.height == e.height) return true;
    if (t_.edges) {
        dev_->wait_idle();
        destroy_targets();
    }
    auto make = [&](VkFormat fmt, const char* name, gfx::Image* out) {
        gfx::ImageDesc id;
        id.format = fmt;
        id.width = e.width;
        id.height = e.height;
        id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        id.name = name;
        std::string why;
        if (dev_->create_image(id, out, &why) != VK_SUCCESS) {
            NXLOG_E(kTag, "%s: %s", name, why.c_str());
            return false;
        }
        return true;
    };
    if (!make(VK_FORMAT_R8G8_UNORM, "smaa edges", &t_.edges) || !make(VK_FORMAT_R8G8B8A8_UNORM, "smaa weights", &t_.weights) ||
        !make(VK_FORMAT_R16G16B16A16_SFLOAT, "smaa out", &t_.out)) {
        destroy_targets();
        return false;
    }
    t_.extent = e;
    return true;
}

VkImageView SmaaScene::run(VkCommandBuffer cb, VkImageView in, VkExtent2D e, int level) {
    const VkImageLayout in_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    struct Bind { VkImageView view; VkImageLayout layout; VkSampler smp; };
    auto draw = [&](VkPipeline pipe, const gfx::Image& target, bool clear, const Bind* binds, uint32_t n) {
        VkDescriptorSet set = sets_[next_];
        next_ = (next_ + 1) % kSetRing;
        VkDescriptorImageInfo infos[3] = {};
        VkWriteDescriptorSet w[3] = {};
        for (uint32_t i = 0; i < n; ++i) {
            infos[i] = {binds[i].smp, binds[i].view, binds[i].layout};
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(dev_->device(), n, w, 0, nullptr);
        // the target's old contents are not needed (the pass writes every texel, or clears it first)
        barrier(cb, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kShaderStages | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = target.view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, e};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
        const VkViewport vpt = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        const VkRect2D scissor = {{0, 0}, e};
        vkCmdSetViewport(cb, 0, 1, &vpt);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        const float metrics[4] = {1.0f / static_cast<float>(e.width), 1.0f / static_cast<float>(e.height), static_cast<float>(e.width), static_cast<float>(e.height)};
        vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof metrics, metrics);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
    };
    auto to_read = [&](const gfx::Image& img, VkImageLayout to) {
        barrier(cb, img.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, to, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                kShaderStages, VK_ACCESS_2_SHADER_READ_BIT);
    };

    const Bind edge_in[1] = {{in, in_layout, nearest_}};
    draw(edge_[level], t_.edges, true, edge_in, 1);
    to_read(t_.edges, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const Bind weight_in[3] = {{t_.edges.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, linear_},
                               {area_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, linear_},
                               {search_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nearest_}};
    draw(weight_[level], t_.weights, true, weight_in, 3);
    to_read(t_.weights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const Bind blend_in[2] = {{in, in_layout, linear_}, {t_.weights.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nearest_}};
    draw(blend_, t_.out, false, blend_in, 2);
    to_read(t_.out, VK_IMAGE_LAYOUT_GENERAL);
    return t_.out.view;
}

VkImageView SmaaScene::apply(VkCommandBuffer cb, VkImageView src, VkExtent2D extent, AaMode mode) {
    if (!dev_ || !src || mode == AaMode::Off || extent.width < 4 || extent.height < 4) return VK_NULL_HANDLE;
    if (!ensure_targets(extent)) return VK_NULL_HANDLE;
    const int level = mode == AaMode::SmaaLow ? 0 : mode == AaMode::SmaaMedium ? 1 : mode == AaMode::SmaaHigh ? 2 : 3;
    VkImageView v = run(cb, src, extent, level);
    ++frames_;
    return v;
}

}  // namespace nxrt::extras
