// SPDX-License-Identifier: GPL-3.0-or-later
// The Android build's forced include (-include, from the Android flag table), in front of every lifted unit. Outside recomp/include:
// no desktop stamp sees this folder, and desktop builds never include it. It brings the lifter headers in itself (their include
// guards make the unit's own #include a no-op) with the Android additions around them.
#pragma once
// The Android flag table also compiles the link step's stubs (pkp_stub.cpp), which do not see the lifter headers: for them this file
// is empty.
#if __has_include(<nxrecomp/cxx.h>)
#define NX_NATIVE_ANDROID 1
#include "nx_native_atomics.h"     // first: its macro must be in place before any compare-and-swap is parsed
#include <arm_neon.h>
// Native FP / NEON (below): the lifter headers' portable versions of these helpers are compiled under other names (the portable
// helpers that call each other keep calling the portable ones), and the lifted code, parsed after this file, calls the native ones.
#define fp_maxmin32 nx_portable_fp_maxmin32
#define fp_maxmin64 nx_portable_fp_maxmin64
#define fp_add32 nx_portable_fp_add32
#define fp_sub32 nx_portable_fp_sub32
#define fp_mul32 nx_portable_fp_mul32
#define fp_div32 nx_portable_fp_div32
#define fp_sqrt32 nx_portable_fp_sqrt32
#define fp_add64 nx_portable_fp_add64
#define fp_sub64 nx_portable_fp_sub64
#define fp_mul64 nx_portable_fp_mul64
#define fp_div64 nx_portable_fp_div64
#define fp_sqrt64 nx_portable_fp_sqrt64
#define fp_fma32 nx_portable_fp_fma32
#define fp_fma64 nx_portable_fp_fma64
#define fp_d2s nx_portable_fp_d2s
#define fp_s2d nx_portable_fp_s2d
#define fp_frint32 nx_portable_fp_frint32
#define fp_frint64 nx_portable_fp_frint64
#define f2s32 nx_portable_f2s32
#define f2u32 nx_portable_f2u32
#define f2s64 nx_portable_f2s64
#define f2u64 nx_portable_f2u64
#define fx_fcmp nx_portable_fx_fcmp
#define v_f3v nx_portable_v_f3v
#define v_f3e nx_portable_v_f3e
#define v_f1v nx_portable_v_f1v
#include <nxrecomp/cxx.h>
#undef fp_maxmin32
#undef fp_maxmin64
#undef fp_add32
#undef fp_sub32
#undef fp_mul32
#undef fp_div32
#undef fp_sqrt32
#undef fp_add64
#undef fp_sub64
#undef fp_mul64
#undef fp_div64
#undef fp_sqrt64
#undef fp_fma32
#undef fp_fma64
#undef fp_d2s
#undef fp_s2d
#undef fp_frint32
#undef fp_frint64
#undef f2s32
#undef f2u32
#undef f2s64
#undef f2u64
#undef fx_fcmp
#undef v_f3v
#undef v_f3e
#undef v_f1v
#include "nx_native_fp.h"

// postlift.py's FCMP rewrite: the FCMP keeps its operands (as doubles: exact for singles) in F.a / F.b, which no other reader of an
// FCMP's flags uses, and a reader reached only by FCMPs tests them directly. N = less, Z = equal, C = not less, V = unordered.
template <typename T>
static inline __attribute__((always_inline)) void nx_fcmp_keep(NxF& f, T a, T b) {
    fx_fcmp(f, a, b);
    f.a = std::bit_cast<uint64_t>(double(a)), f.b = std::bit_cast<uint64_t>(double(b));
}
static inline __attribute__((always_inline)) int nx_cc_fcmp(const NxF& f, int cc) {
    double a = std::bit_cast<double>(f.a), b = std::bit_cast<double>(f.b);
    switch (cc) {
    case 0: return a == b;                          // EQ
    case 1: return !(a == b);                       // NE
    case 2: return !(a < b);                        // CS: greater, equal or unordered
    case 3: return a < b;                           // CC
    case 4: return a < b;                           // MI
    case 5: return !(a < b);                        // PL
    case 6: return __builtin_isunordered(a, b);     // VS
    case 7: return !__builtin_isunordered(a, b);    // VC
    case 8: return !(a <= b);                       // HI: greater or unordered
    case 9: return a <= b;                          // LS
    case 10: return a >= b;                         // GE
    case 11: return !(a >= b);                      // LT: less or unordered
    case 12: return a > b;                          // GT
    case 13: return !(a > b);                       // LE: less, equal or unordered
    default: return 1;
    }
}
#endif
