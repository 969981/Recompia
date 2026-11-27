// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-1 (FIPS 180-4) for the shader cache's keys (nvn_shaders.h Sha1): the portable loop and the CPU's SHA instructions (x86 SHA-NI,
// ARMv8 SHA-1) for whole blocks, same digest. Moved from nvn_shaders.cpp; sha1_hex and shader_hash stay there.
#include "nvn_shaders.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <intrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#if defined(__linux__)
#include <asm/hwcap.h>   // HWCAP_SHA1
#include <sys/auxv.h>    // getauxval
#endif
#endif

namespace nxrt::nvn::be {

// ---- SHA-1 ----------------------------------------------------------------------------------------------------------

namespace {

inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// The SHA instructions (Intel since Ice Lake / Alder Lake, AMD since Zen): the same digest as block() at several times the speed. The
// shader cache keys on this hash, and the game hashes every stage of every program it links (over a second of the boot on the portable
// loop). Intrinsics, no inline assembly; chosen at run time from CPUID.
// x86: SHA-NI. arm64: the ARMv8 SHA-1 instructions, which every Apple silicon core has.
#if defined(__x86_64__) || defined(_M_X64)
bool have_shani() {
    static const bool yes = [] {
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7) return false;
        __cpuidex(r, 1, 0);
        const bool sse41 = (r[2] >> 19) & 1, ssse3 = (r[2] >> 9) & 1;
        __cpuidex(r, 7, 0);
        return sse41 && ssse3 && ((r[1] >> 29) & 1);
    }();
    return yes;
}

template <int F>
__attribute__((target("sha,sse4.1,ssse3"))) inline __m128i rnds4(__m128i abcd, __m128i wk) { return _mm_sha1rnds4_epu32(abcd, wk, F); }

__attribute__((target("sha,sse4.1,ssse3"))) void sha1_blocks_shani(uint32_t h[5], const uint8_t* data, size_t blocks) {
    const __m128i kSwap = _mm_set_epi64x(0x0001020304050607ll, 0x08090a0b0c0d0e0fll);
    __m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(h)), 0x1B);
    __m128i e0 = _mm_set_epi32(static_cast<int>(h[4]), 0, 0, 0);
    for (; blocks; --blocks, data += 64) {
        const __m128i abcd_save = abcd, e_save = e0;
        __m128i m[4];
        for (int i = 0; i < 4; i++) m[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), kSwap);
        __m128i eprev = e0;
        for (int g = 0; g < 20; g++) {
            const __m128i wk = g == 0 ? _mm_add_epi32(e0, m[0]) : _mm_sha1nexte_epu32(eprev, m[g & 3]);
            eprev = abcd;
            switch (g / 5) {   // the round function is an immediate
                case 0: abcd = rnds4<0>(abcd, wk); break;
                case 1: abcd = rnds4<1>(abcd, wk); break;
                case 2: abcd = rnds4<2>(abcd, wk); break;
                default: abcd = rnds4<3>(abcd, wk); break;
            }
            if (g + 4 < 20)   // W[4(g+4) ..] from the four groups before it; it takes m[g]'s slot, which this group has used
                m[g & 3] = _mm_sha1msg2_epu32(_mm_xor_si128(_mm_sha1msg1_epu32(m[g & 3], m[(g + 1) & 3]), m[(g + 2) & 3]), m[(g + 3) & 3]);
        }
        e0 = _mm_sha1nexte_epu32(eprev, e_save);
        abcd = _mm_add_epi32(abcd, abcd_save);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(h), _mm_shuffle_epi32(abcd, 0x1B));
    h[4] = static_cast<uint32_t>(_mm_extract_epi32(e0, 3));
}
#elif defined(__aarch64__)
// ARMv8 SHA-1 instructions: the same digest as block(), about four times faster. Every Apple silicon core has them, and so does
// the Switch 2's A78C; on arm64 Linux they are optional (an early Cortex-A53 or A72 without the crypto extension), so the kernel
// is asked there.
bool have_shani() {
#if defined(__APPLE__)
    return true;
#elif defined(__linux__)
    static const bool yes = (getauxval(AT_HWCAP) & HWCAP_SHA1) != 0;
    return yes;
#else
    return true;
#endif
}
// The SHA-1 instructions are in the optional crypto extension, so the function carries the feature (as the x86 path does with its
// target attribute) and have_shani() decides at run time whether it may be called.
#if defined(__clang__)
#define NX_SHA1_TARGET __attribute__((target("sha2")))
#else
#define NX_SHA1_TARGET __attribute__((target("arch=armv8-a+crypto")))
#endif
NX_SHA1_TARGET void sha1_blocks_shani(uint32_t h[5], const uint8_t* data, size_t blocks) {
    uint32x4_t abcd = vld1q_u32(h);
    uint32_t e0 = h[4];
    const uint32x4_t k0 = vdupq_n_u32(0x5A827999), k1 = vdupq_n_u32(0x6ED9EBA1), k2 = vdupq_n_u32(0x8F1BBCDC), k3 = vdupq_n_u32(0xCA62C1D6);
    for (; blocks; --blocks, data += 64) {
        const uint32x4_t abcd_save = abcd;
        const uint32_t e_save = e0;
        uint32x4_t m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data)));
        uint32x4_t m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16)));
        uint32x4_t m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32)));
        uint32x4_t m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48)));
        uint32x4_t t0 = vaddq_u32(m0, k0), t1 = vaddq_u32(m1, k0);
        uint32_t e1;
        // rounds 0-3 .. 76-79, four per step; the message schedule with sha1su0/su1 (the ARM ACLE reference ordering)
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k0); m0 = vsha1su0q_u32(m0, m1, m2);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k0); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k0); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k1); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k1); m3 = vsha1su1q_u32(m3, m2); m0 = vsha1su0q_u32(m0, m1, m2);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k1); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k1); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k1); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k2); m3 = vsha1su1q_u32(m3, m2); m0 = vsha1su0q_u32(m0, m1, m2);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k2); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k2); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k2); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k2); m3 = vsha1su1q_u32(m3, m2); m0 = vsha1su0q_u32(m0, m1, m2);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k3); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k3); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k3); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k3); m3 = vsha1su1q_u32(m3, m2);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k3);
        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0);
        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1);
        e0 += e_save;
        abcd = vaddq_u32(abcd_save, abcd);
    }
    vst1q_u32(h, abcd);
    h[4] = e0;
}
#else
bool have_shani() { return false; }
#endif

std::atomic<bool> g_shani_off{false};   // Sha1::force_portable (tests)

}  // namespace

Sha1::Sha1() : h_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0} {}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
}

void Sha1::update(const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    bytes_ += size;
    if (used_) {
        size_t n = std::min(size, 64 - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n, p += n, size -= n;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
    if (size >= 64 && have_shani() && !g_shani_off.load(std::memory_order_relaxed)) {
        sha1_blocks_shani(h_, p, size / 64);
        p += size & ~size_t{63};
        size &= 63;
    }
#endif
    for (; size >= 64; p += 64, size -= 64) block(p);
    std::memcpy(buf_, p, size);
    used_ = size;
}

bool Sha1::shani_available() { return have_shani(); }
void Sha1::force_portable(bool on) { g_shani_off.store(on, std::memory_order_relaxed); }

void Sha1::finish(uint8_t digest[20]) {
    uint64_t bits = bytes_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (used_ != 56) update(&pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 20; i++) digest[i] = uint8_t(h_[i / 4] >> (24 - 8 * (i % 4)));
}

}  // namespace nxrt::nvn::be
