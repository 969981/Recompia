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
#include <nxrecomp/cxx.h>

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
