// SPDX-License-Identifier: GPL-3.0-or-later
// The helpers under test, compiled twice by build.sh: once as on any host (-DNX_PORTABLE_FP, namespace gen) and once with the
// AArch64 fast paths of fpexact.h / neon.h / locals.h (namespace nat). difftest.cpp runs both on the same inputs.
#include <nxrecomp/cxx.h>
#include <utility>
#include "impl.h"

namespace IMPL_NS {
namespace {
template <int OP, int DBL, int N> void f3v(V128 *d, const V128 *a, const V128 *b, int) { v_f3v(d, a, b, OP, DBL, N); }
template <int OP, int DBL, int N> void f3e(V128 *d, const V128 *a, const V128 *b, int idx) {
    switch (idx) {               // the lifter passes the element index as a literal
    case 0: v_f3e(d, a, b, 0, OP, DBL, N); break;
    case 1: v_f3e(d, a, b, 1, OP, DBL, N); break;
    case 2: v_f3e(d, a, b, 2, OP, DBL, N); break;
    default: v_f3e(d, a, b, 3, OP, DBL, N); break;
    }
}
template <int OP, int DBL, int N> void f1v(V128 *d, const V128 *a, const V128 *, int) { v_f1v(d, a, OP, DBL, N); }
}  // namespace

// ops x shapes: shape 0..2 single n = 1, 2, 4; shape 3..4 double n = 1, 2 (dbl flag fixed up below)
template <int OP, int SH> void f3v_sh(V128 *d, const V128 *a, const V128 *b, int i) {
    if constexpr (SH < 3) f3v<OP, 0, SH == 0 ? 1 : SH == 1 ? 2 : 4>(d, a, b, i); else f3v<OP, 1, SH == 3 ? 1 : 2>(d, a, b, i);
}
template <int OP, int SH> void f3e_sh(V128 *d, const V128 *a, const V128 *b, int i) {
    if constexpr (SH < 3) f3e<OP, 0, SH == 0 ? 1 : SH == 1 ? 2 : 4>(d, a, b, i); else f3e<OP, 1, SH == 3 ? 1 : 2>(d, a, b, i & 1);
}
template <int OP, int SH> void f1v_sh(V128 *d, const V128 *a, const V128 *b, int i) {
    if constexpr (SH < 3) f1v<OP, 0, SH == 0 ? 1 : SH == 1 ? 2 : 4>(d, a, b, i); else f1v<OP, 1, SH == 3 ? 1 : 2>(d, a, b, i);
}
template <template <int, int> class W, std::size_t... I>
constexpr std::array<VecFn, sizeof...(I)> table(std::index_sequence<I...>) { return {{&W<int(I / 5), int(I % 5)>::fn...}}; }
template <int OP, int SH> struct W3v { static void fn(V128 *d, const V128 *a, const V128 *b, int i) { f3v_sh<OP, SH>(d, a, b, i); } };
template <int OP, int SH> struct W3e { static void fn(V128 *d, const V128 *a, const V128 *b, int i) { f3e_sh<OP, SH>(d, a, b, i); } };
template <int OP, int SH> struct W1v { static void fn(V128 *d, const V128 *a, const V128 *b, int i) { f1v_sh<OP, SH>(d, a, b, i); } };

const Impl impl = {
    table<W3v>(std::make_index_sequence<17 * 5>()),
    table<W3e>(std::make_index_sequence<16 * 5>()),        // by element: every op but ADDP
    table<W1v>(std::make_index_sequence<14 * 5>()),
    {[](float a, float b) { return fp_add32(a, b); }, [](float a, float b) { return fp_sub32(a, b); },
     [](float a, float b) { return fp_mul32(a, b); }, [](float a, float b) { return fp_div32(a, b); },
     [](float a, float b) { return fp_sqrt32(a); },
     [](float a, float b) { return fp_maxmin32(a, b, 0, 0); }, [](float a, float b) { return fp_maxmin32(a, b, 1, 0); },
     [](float a, float b) { return fp_maxmin32(a, b, 0, 1); }, [](float a, float b) { return fp_maxmin32(a, b, 1, 1); },
     [](float a, float b) { uint64_t u = (uint64_t(fp_bits32(a)) << 32) | fp_bits32(b); return fp_d2s(fp_from_bits64(u)); }},
    {[](double a, double b) { return fp_add64(a, b); }, [](double a, double b) { return fp_sub64(a, b); },
     [](double a, double b) { return fp_mul64(a, b); }, [](double a, double b) { return fp_div64(a, b); },
     [](double a, double b) { return fp_sqrt64(a); },
     [](double a, double b) { return fp_maxmin64(a, b, 0, 0); }, [](double a, double b) { return fp_maxmin64(a, b, 1, 0); },
     [](double a, double b) { return fp_maxmin64(a, b, 0, 1); }, [](double a, double b) { return fp_maxmin64(a, b, 1, 1); },
     [](double a, double b) { return fp_s2d(fp_from_bits32(uint32_t(fp_bits64(a)))); }},
    [](float n, float m, float a) { return fp_fma32(n, m, a); },
    [](double n, double m, double a) { return fp_fma64(n, m, a); },
    [](float a, float b) { NxF F = {0, 0, 0, 0, 0, 0}; fx_fcmp(F, (double)a, (double)b); return fx_get_nzcv(F); },
    [](double a, double b) { NxF F = {0, 0, 0, 0, 0, 0}; fx_fcmp(F, a, b); return fx_get_nzcv(F); },
    [](float a, float b) {   // every condition a reader may ask, packed (catches a fold that is wrong for one cc only)
        NxF F = {0, 0, 0, 0, 0, 0};
        fx_fcmp(F, (double)a, (double)b);
        uint32_t r = 0;
        for (int cc = 0; cc < 16; cc++) r |= uint32_t(fx_cond(F, cc) ? 1 : 0) << cc;
        return uint64_t(r);
    },
#ifdef NX_NATIVE_ANDROID
    [](float a, float b) {   // postlift.py's operand form of the 16 conditions (Android forced include)
        NxF F = {0, 0, 0, 0, 0, 0};
        nx_fcmp_keep(F, (double)a, (double)b);
        uint32_t r = 0;
        for (int cc = 0; cc < 16; cc++) r |= uint32_t(nx_cc_fcmp(F, cc) ? 1 : 0) << cc;
        return uint64_t(r);
    },
#else
    nullptr,
#endif
    {[](float a, float) { return fp_frint32(a, 0); }, [](float a, float) { return fp_frint32(a, 1); },
     [](float a, float) { return fp_frint32(a, 2); }, [](float a, float) { return fp_frint32(a, 3); },
     [](float a, float) { return fp_frint32(a, 4); }},
    {[](double a, double) { return fp_frint64(a, 0); }, [](double a, double) { return fp_frint64(a, 1); },
     [](double a, double) { return fp_frint64(a, 2); }, [](double a, double) { return fp_frint64(a, 3); },
     [](double a, double) { return fp_frint64(a, 4); }},
    {[](double v) { return uint64_t(f2s32(v)); }, [](double v) { return uint64_t(f2u32(v)); },
     [](double v) { return f2s64(v); }, [](double v) { return f2u64(v); }},
    [](uint8_t *p, uint8_t *e, uint8_t d) { return __atomic_compare_exchange_n(p, e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); },
    [](uint16_t *p, uint16_t *e, uint16_t d) { return __atomic_compare_exchange_n(p, e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); },
    [](uint32_t *p, uint32_t *e, uint32_t d) { return __atomic_compare_exchange_n(p, e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); },
    [](uint64_t *p, uint64_t *e, uint64_t d) { return __atomic_compare_exchange_n(p, e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); },
};
}  // namespace IMPL_NS
