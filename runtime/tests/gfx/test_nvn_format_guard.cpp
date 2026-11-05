// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for nvn_format_guard.cpp with a fake feature table (no Vulkan device), and the RGB10A2SN widening of nvn_texdata.cpp.
#include "nvn_format_guard.h"
#include "nvn_format.h"
#include "nvn_texdata.h"
#include "test_framework.h"

#include <cstring>
#include <map>

NXTEST_GROUP(gfx_nvn_format_guard)

using namespace nxrt::nvn;

namespace {

struct Fake {
    std::map<VkFormat, FormatFeatureBits> t;
    int probes = 0;
    FormatGuard make() {
        return FormatGuard([this](VkFormat f) -> std::optional<FormatFeatureBits> {
            ++probes;
            const auto it = t.find(f);
            return it == t.end() ? FormatFeatureBits{} : it->second;
        });
    }
};

constexpr VkFormatFeatureFlags kBlend = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
constexpr VkFormatFeatureFlags kLin = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
constexpr VkFormatFeatureFlags kVtx = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;

}  // namespace

NXTEST(blend_follows_the_bit) {
    Fake f;
    f.t[VK_FORMAT_R8G8B8A8_UNORM] = {kBlend, 0};
    f.t[VK_FORMAT_R32G32B32A32_SFLOAT] = {0, 0};
    FormatGuard g = f.make();
    CHECK(g.blend_allowed(VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(!g.blend_allowed(VK_FORMAT_R32G32B32A32_SFLOAT));
    CHECK(g.blend_allowed(VK_FORMAT_UNDEFINED));
    CHECK_EQ(g.triggered(), static_cast<size_t>(1));
    CHECK(!g.blend_allowed(VK_FORMAT_R32G32B32A32_SFLOAT));   // logged once
    CHECK_EQ(g.triggered(), static_cast<size_t>(1));
}

NXTEST(features_are_cached) {
    Fake f;
    f.t[VK_FORMAT_R8_UNORM] = {kBlend | kLin, 0};
    FormatGuard g = f.make();
    for (int i = 0; i < 5; ++i) {
        CHECK(g.blend_allowed(VK_FORMAT_R8_UNORM));
        CHECK(g.linear_filter_allowed(VK_FORMAT_R8_UNORM));
    }
    CHECK_EQ(f.probes, 1);
}

NXTEST(sampler_goes_nearest_without_linear) {
    Fake f;
    f.t[VK_FORMAT_R32_SFLOAT] = {kBlend, 0};
    f.t[VK_FORMAT_R8G8B8A8_UNORM] = {kLin, 0};
    FormatGuard g = f.make();
    int mn = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, mg = NVN_MAG_FILTER_LINEAR;
    float an = 8.0f;
    CHECK(g.limit_sampler(VK_FORMAT_R32_SFLOAT, &mn, &mg, &an));
    CHECK_EQ(mn, static_cast<int>(NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR));
    CHECK_EQ(mg, static_cast<int>(NVN_MAG_FILTER_NEAREST));
    CHECK(an == 1.0f);
    mn = NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST; mg = NVN_MAG_FILTER_NEAREST; an = 1.0f;
    CHECK(g.limit_sampler(VK_FORMAT_R32_SFLOAT, &mn, &mg, &an));
    CHECK_EQ(mn, static_cast<int>(NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST));
    mn = NVN_MIN_FILTER_LINEAR; mg = NVN_MAG_FILTER_LINEAR; an = 4.0f;
    CHECK(!g.limit_sampler(VK_FORMAT_R8G8B8A8_UNORM, &mn, &mg, &an));   // supported: untouched
    CHECK_EQ(mn, static_cast<int>(NVN_MIN_FILTER_LINEAR));
    CHECK(an == 4.0f);
    mn = NVN_MIN_FILTER_NEAREST; mg = NVN_MAG_FILTER_NEAREST;
    CHECK(!g.limit_sampler(VK_FORMAT_R32_SFLOAT, &mn, &mg, &an));       // already nearest: nothing to do
}

NXTEST(vertex_fetch_fallbacks) {
    Fake f;
    f.t[VK_FORMAT_R32G32B32A32_SFLOAT] = {0, kVtx};
    f.t[VK_FORMAT_R8G8B8A8_UNORM] = {0, kVtx};
    f.t[VK_FORMAT_R32G32_SFLOAT] = {0, kVtx};
    FormatGuard g = f.make();
    CHECK_EQ(g.vertex_fetch_format(VK_FORMAT_R32G32_SFLOAT), VK_FORMAT_R32G32_SFLOAT);
    CHECK_EQ(g.triggered(), static_cast<size_t>(0));
    CHECK_EQ(g.vertex_fetch_format(VK_FORMAT_R32G32B32_SFLOAT), VK_FORMAT_R32G32B32A32_SFLOAT);   // 3 -> 4 components
    CHECK_EQ(g.vertex_fetch_format(VK_FORMAT_R8G8B8_UNORM), VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(g.vertex_fetch_format(VK_FORMAT_A2B10G10R10_SNORM_PACK32), VK_FORMAT_UNDEFINED);      // no sibling: left out
    CHECK_EQ(g.vertex_fetch_format(VK_FORMAT_R16G16B16_SFLOAT), VK_FORMAT_UNDEFINED);              // sibling lacks it too
    CHECK_EQ(g.triggered(), static_cast<size_t>(4));
}

NXTEST(full_featured_device_changes_nothing) {
    Fake f;
    FormatGuard g([](VkFormat) -> std::optional<FormatFeatureBits> { return FormatFeatureBits{~0u, ~0u}; });
    int mn = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, mg = NVN_MAG_FILTER_LINEAR;
    float an = 16.0f;
    CHECK(g.blend_allowed(VK_FORMAT_A2B10G10R10_UNORM_PACK32));
    CHECK(!g.limit_sampler(VK_FORMAT_R16G16B16A16_SFLOAT, &mn, &mg, &an));
    CHECK_EQ(g.vertex_fetch_format(VK_FORMAT_R8G8B8_UNORM), VK_FORMAT_R8G8B8_UNORM);
    CHECK_EQ(g.triggered(), static_cast<size_t>(0));
}

NXTEST(unknown_device_allows_everything_and_is_not_cached) {
    int n = 0;
    FormatGuard g([&n](VkFormat) -> std::optional<FormatFeatureBits> {
        return ++n == 1 ? std::nullopt : std::optional<FormatFeatureBits>(FormatFeatureBits{});
    });
    CHECK(g.blend_allowed(VK_FORMAT_R8_UNORM));    // no device yet
    CHECK(!g.blend_allowed(VK_FORMAT_R8_UNORM));   // device known, no blend bit: probed again
}

NXTEST(rgb10a2sn_has_a_wide_fallback) {
    const FormatInfo* fi = format_info(NVN_FORMAT_RGB10A2SN);
    REQUIRE(fi != nullptr);
    CHECK_EQ(fi->vk, VK_FORMAT_A2B10G10R10_SNORM_PACK32);
    CHECK_EQ(fi->vk_fallback, VK_FORMAT_R16G16B16A16_SNORM);
}

NXTEST(snorm10_widening) {
    // R = 511 (max), G = -512 (clamps to -1), B = 0, A = 1 (max of 2 bits)
    const uint32_t px[2] = {511u | (0x200u << 10) | (0u << 20) | (1u << 30), (0x1FFu << 20) | (0x2u << 30)};   // second: B = 511, A = -2 (-1)
    int16_t out[8] = {};
    be::snorm10_to_snorm16(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<uint8_t*>(out), 2);
    CHECK_EQ(out[0], 32767);
    CHECK_EQ(out[1], -32767);
    CHECK_EQ(out[2], 0);
    CHECK_EQ(out[3], 32767);
    CHECK_EQ(out[4], 0);
    CHECK_EQ(out[5], 0);
    CHECK_EQ(out[6], 32767);
    CHECK_EQ(out[7], -32767);
}
