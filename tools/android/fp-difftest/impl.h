// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <nxrecomp/ctx.h>

using VecFn = void (*)(V128 *d, const V128 *a, const V128 *b, int idx);
using F32Fn = float (*)(float, float);
using F64Fn = double (*)(double, double);
struct Impl {
    std::array<VecFn, 17 * 5> f3v;   // [op * 5 + shape]; shapes: single n = 1, 2, 4; double n = 1, 2
    std::array<VecFn, 16 * 5> f3e;
    std::array<VecFn, 14 * 5> f1v;
    std::array<F32Fn, 10> s32;       // add sub mul div sqrt max min maxnm minnm d2s
    std::array<F64Fn, 10> s64;       // add sub mul div sqrt max min maxnm minnm s2d
    float (*fma32)(float, float, float);
    double (*fma64)(double, double, double);
    uint64_t (*fcmp32)(float, float);
    uint64_t (*fcmp64)(double, double);
    uint64_t (*fcond32)(float, float);
    uint64_t (*fccop32)(float, float);   // the same conditions from the operands (postlift.py, native build only)
    std::array<F32Fn, 5> frint32;    // fp_frint32 modes 0..4 (second operand unused)
    std::array<F64Fn, 5> frint64;
    std::array<uint64_t (*)(double), 4> cvt;   // f2s32 f2u32 f2s64 f2u64
    // __atomic_compare_exchange_n as the lifted code writes it (seq_cst, strong) on 1, 2, 4, 8 bytes; native build: through the
    // Android forced include
    bool (*cas8)(uint8_t *, uint8_t *, uint8_t);
    bool (*cas16)(uint16_t *, uint16_t *, uint16_t);
    bool (*cas32)(uint32_t *, uint32_t *, uint32_t);
    bool (*cas64)(uint64_t *, uint64_t *, uint64_t);
};
namespace gen { extern const Impl impl; }
namespace nat { extern const Impl impl; }
