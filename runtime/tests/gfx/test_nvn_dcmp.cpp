// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for nvn_dcmp.h: the red value of texel 0 for the formats of the game's default textures, and the by-value stand-in cache.
#include "nvn_dcmp.h"
#include "test_framework.h"

NXTEST_GROUP(gfx_nvn_dcmp)

using namespace nxrt::nvn::be;

NXTEST(red_of_default_textures) {
    bool ok = false;
    const uint8_t black[4] = {0, 0, 0, 255}, white[4] = {255, 255, 255, 255}, grey[4] = {128, 0, 0, 0};
    CHECK_EQ(dcmp_red(VK_FORMAT_R8G8B8A8_UNORM, black, &ok), 0.0f);
    CHECK(ok);
    CHECK_EQ(dcmp_red(VK_FORMAT_R8G8B8A8_UNORM, white, &ok), 1.0f);
    CHECK_EQ(dcmp_red(VK_FORMAT_R8G8B8A8_SRGB, white, &ok), 1.0f);
    CHECK_EQ(dcmp_red(VK_FORMAT_R8G8B8A8_SRGB, black, &ok), 0.0f);
    CHECK(std::fabs(dcmp_red(VK_FORMAT_R8G8B8A8_UNORM, grey, &ok) - 128.0f / 255.0f) < 1e-6f);
    const uint8_t bgra[4] = {0, 0, 255, 255};   // B G R A: red is byte 2
    CHECK_EQ(dcmp_red(VK_FORMAT_B8G8R8A8_UNORM, bgra, &ok), 1.0f);
    const uint8_t half[2] = {0x00, 0x38};       // 0.5 as an IEEE half
    CHECK_EQ(dcmp_red(VK_FORMAT_R16_SFLOAT, half, &ok), 0.5f);
}

NXTEST(unknown_format_is_lit) {
    bool ok = true;
    const uint8_t px[16] = {};
    CHECK_EQ(dcmp_red(VK_FORMAT_BC7_UNORM_BLOCK, px, &ok), 1.0f);
    CHECK(!ok);
}

NXTEST(value_cache) {
    DcmpValues c;
    bool added = false;
    CHECK_EQ(c.slot(0.0f, &added), 0);
    CHECK(added);
    CHECK_EQ(c.slot(1.0f, &added), 1);
    CHECK(added);
    CHECK_EQ(c.slot(0.0f, &added), 0);   // same value, same slot
    CHECK(!added);
    for (int i = 2; i < kDcmpMaxValues; ++i) c.slot(0.1f * static_cast<float>(i));
    CHECK_EQ(c.n, kDcmpMaxValues);
    CHECK_EQ(c.slot(0.99f, &added), 1);  // full: the nearest kept value
    CHECK(!added);
    CHECK_EQ(c.n, kDcmpMaxValues);
}
