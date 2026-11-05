// SPDX-License-Identifier: GPL-3.0-or-later
// NVN driver constant bank c[0] (src/gfx/nvn/nvn_driver_bank.{h,cpp}). The expected words are the ones the translated game shaders
// read (spirv-cross of the shader cache: c0[row].component, row = byte / 16) with the table counts of their control sections:
//   9a1c15151afc6b03 / d0d1504b06322045 (light culling)  ubos 1 ssbos 3 textures 4   reads c0[2].xyz c0[3].z c0[4].x
//   2328277f17597073 / fd78c59750e1af5d (GPU particles)  ubos 1 ssbos 4 textures 3   reads c0[2].z c0[5].z
//   06ab7e6b7ae67b1c (volume field)                     ubos 3 ssbos 2 textures 32  reads c0[4].xyz
// and the results.tsv storage words of the same shaders (sb[c0+0x8] = byte 0x20 = storage slot 0 of the first two kinds).
#include <cstdint>
#include <cstring>

#include "nvn_driver_bank.h"
#include "nvn_shaders.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_driver_bank)

using namespace nxrt::nvn::be;

namespace {

ShaderControl nvn2_cs(uint8_t ubos, uint8_t ssbos, uint8_t textures, uint8_t images) {
    // a Switch 2 compute control section as read_control parses it: magic, version 201, stage u16 +0x56 = 5, counts at +0xb1..+0xb6
    uint8_t ctrl[0xd8] = {};
    const uint32_t magic = kShaderControlMagic, version = 201;
    std::memcpy(ctrl, &magic, 4);
    std::memcpy(ctrl + 4, &version, 4);
    ctrl[0x56] = 5;
    ctrl[0xb1] = ubos;
    ctrl[0xb2] = ssbos;
    ctrl[0xb3] = textures;
    ctrl[0xb6] = images;
    return read_control(ctrl);
}

uint32_t word(const uint8_t* d, int row, int comp) {
    uint32_t v;
    std::memcpy(&v, d + row * 16 + comp * 4, 4);
    return v;
}

struct Bindings {
    DriverRange ubo[16], ssbo[16];
    uint64_t tex[64] = {}, img[16] = {};
    Bindings() {
        for (int n = 0; n < 16; ++n) {
            ubo[n] = {0x8100000000ull + 0x10000ull * n, 0x100u + n};
            ssbo[n] = {0x8200000000ull + 0x10000ull * n + 0x40, 0x2000u + 0x10u * n};
            img[n] = 0x900u + n;
        }
        for (int n = 0; n < 64; ++n) tex[n] = (0x123ull << 32) | (0x500u + n);   // the high word is not the hardware handle
    }
};

}  // namespace

NXTEST(control_counts_give_the_table) {
    const ShaderControl c = nvn2_cs(3, 2, 32, 0);
    CHECK(c.nvn2);
    CHECK_EQ(c.stage, 5);
    const Nvn2ComputeTable t = nvn2_compute_table(c);
    CHECK_EQ(t.ubo0, 0x10);
    CHECK_EQ(t.ssbo0, 0x40);
    CHECK_EQ(t.tex0, 0x60);
    CHECK_EQ(t.img0, 0x160);
    CHECK_EQ(t.end, 0x160);   // control u16 +0xba of 06ab7e6b7ae67b1c
}

NXTEST(light_culling_reads_storage_sizes) {
    Bindings b;
    uint8_t d[kDriverBankBytes];
    std::memset(d, 0xcc, sizeof d);
    const uint32_t n = fill_driver_bank_nvn2_compute(d, nvn2_cs(1, 3, 4, 0), b.ubo, b.ssbo, b.tex, b.img);
    CHECK_EQ(n, 0x70u);   // control +0xba
    for (int i = 0; i < 16; ++i) CHECK_EQ(d[i], 0);   // header
    // uniform block 0 at row 1
    CHECK_EQ(word(d, 1, 0), 0u);
    CHECK_EQ(word(d, 1, 1), 0x81u);
    CHECK_EQ(word(d, 1, 2), 0x100u);
    // storage slot 0 at row 2: c0[2].x / .y address, c0[2].z size (the shader's `max(c0[2].z, 0) >> 2` element bound)
    CHECK_EQ(word(d, 2, 0), 0x40u);
    CHECK_EQ(word(d, 2, 1), 0x82u);
    CHECK_EQ(word(d, 2, 2), 0x2000u);
    CHECK_EQ(word(d, 2, 3), 0u);
    CHECK_EQ(word(d, 3, 2), 0x2010u);       // c0[3].z: slot 1's size
    CHECK_EQ(word(d, 4, 0), 0x20040u);      // c0[4].x: slot 2's address
    // textures from 0x50 (words 20, 22, 24, 26 in results.tsv): the low 32 bits of the handle
    uint32_t h;
    std::memcpy(&h, d + 0x50, 4);
    CHECK_EQ(h, 0x500u);
    std::memcpy(&h, d + 0x54, 4);
    CHECK_EQ(h, 0u);
    std::memcpy(&h, d + 0x68, 4);
    CHECK_EQ(h, 0x503u);
    for (uint32_t i = 0x70; i < kDriverBankBytes; ++i) CHECK_EQ(d[i], 0);   // past the table: zeros
}

NXTEST(particles_read_first_and_last_storage_size) {
    Bindings b;
    uint8_t d[kDriverBankBytes];
    fill_driver_bank_nvn2_compute(d, nvn2_cs(1, 4, 3, 0), b.ubo, b.ssbo, b.tex, b.img);
    CHECK_EQ(word(d, 2, 2), 0x2000u);   // c0[2].z
    CHECK_EQ(word(d, 5, 2), 0x2030u);   // c0[5].z: storage slot 3
    uint32_t h;
    std::memcpy(&h, d + 0x60, 4);       // texture word 24
    CHECK_EQ(h, 0x500u);
}

NXTEST(volume_field_reads_storage_after_three_uniform_blocks) {
    Bindings b;
    uint8_t d[kDriverBankBytes];
    fill_driver_bank_nvn2_compute(d, nvn2_cs(3, 2, 32, 0), b.ubo, b.ssbo, b.tex, b.img);
    CHECK_EQ(word(d, 3, 2), 0x102u);    // uniform block 2 (c[5]) at row 3
    CHECK_EQ(word(d, 4, 0), 0x40u);     // c0[4]: storage slot 0 (sb[c0+0x10])
    CHECK_EQ(word(d, 4, 1), 0x82u);
    CHECK_EQ(word(d, 4, 2), 0x2000u);
    CHECK_EQ(word(d, 5, 0), 0x10040u);  // storage slot 1 (sb[c0+0x14])
}

NXTEST(images_follow_textures) {
    Bindings b;
    uint8_t d[kDriverBankBytes];
    const uint32_t n = fill_driver_bank_nvn2_compute(d, nvn2_cs(0, 1, 2, 2), b.ubo, b.ssbo, b.tex, b.img);
    CHECK_EQ(n, 0x40u);
    CHECK_EQ(word(d, 1, 2), 0x2000u);   // no uniform blocks: storage slot 0 at row 1
    uint32_t h;
    std::memcpy(&h, d + 0x28, 4);
    CHECK_EQ(h, 0x501u);
    std::memcpy(&h, d + 0x30, 4);
    CHECK_EQ(h, 0x900u);
    std::memcpy(&h, d + 0x38, 4);
    CHECK_EQ(h, 0x901u);
}

NXTEST(counts_past_the_replay_slots_stay_in_the_bank) {
    Bindings b;
    uint8_t d[kDriverBankBytes];
    const uint32_t n = fill_driver_bank_nvn2_compute(d, nvn2_cs(255, 255, 255, 255), b.ubo, b.ssbo, b.tex, b.img);
    CHECK_EQ(n, kDriverBankBytes);
    CHECK_EQ(word(d, 16, 2), 0x10fu);   // uniform block 15
    CHECK_EQ(word(d, 17, 2), 0u);       // block 16: the replay keeps 16
}

// Graphics: the storage buffer tables the translator reports (vertex sb[c0+0x50] = byte 0x140 = slot 3; fragment row 0x55 = byte
// 0x550 = slot 4), the point size and c0[100].y = the base instance.
NXTEST(graphics_tables_point_size_and_base_instance) {
    Bindings b;
    uint8_t d[kDriverBankBytes];
    const uint32_t n = fill_driver_bank_graphics(d, 0, b.ssbo, 7);
    CHECK_EQ(n, 0x648u);
    CHECK_EQ(word(d, 0x14, 0), 0x30040u);   // vertex slot 3 at 0x140
    CHECK_EQ(word(d, 0x14, 2), 0x2030u);
    CHECK_EQ(word(d, 100, 1), 7u);          // c0[100].y
    float ps;
    std::memcpy(&ps, d + 0x610, 4);
    CHECK_EQ(ps, 64.0f);
    for (int row = 0x21; row < 0x61; ++row) CHECK_EQ(word(d, row, 0), 0u);   // other stages' tables stay zero
    fill_driver_bank_graphics(d, 1, b.ssbo, 0);
    CHECK_EQ(word(d, 0x55, 0), 0x40040u);   // fragment slot 4 at 0x550
    CHECK_EQ(word(d, 0x11, 0), 0u);         // no vertex table
    CHECK_EQ(word(d, 100, 1), 0u);
    fill_driver_bank_graphics(d, 2, b.ssbo, 0);
    CHECK_EQ(word(d, 0x41, 0), 0x40u);      // geometry slot 0 at 0x410 (arc0017_s1: sb[c0+0x104])
}
