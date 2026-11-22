// SPDX-License-Identifier: GPL-3.0-or-later
// Android build only (included by nx_native.h after the lifter headers): the floating-point and Advanced SIMD helpers of
// nxrecomp/fpexact.h, neon.h, locals.h and ctx.h as the host's own instructions. The host is AArch64 like the guest and its FPCR
// stays at the reset value (0: round to nearest even, no flush-to-zero, DN = 0, AH = 0; the runtime never writes it), so each
// instruction gives exactly the result the portable code gives, NaN propagation and signs included (tools/android/fp-difftest
// compared both bit for bit on a Tensor G2 (Mali-G710): 0 mismatches in 500,000 inputs per helper variant). The portable versions stay
// compiled as nx_portable_<name> (nx_native.h) and are what the other portable helpers call.
#pragma once

#define NX_NATIVE_FP 1
#define NX_NEON_INLINE static inline __attribute__((always_inline))

// ---- scalar (fpexact.h) ------------------------------------------------------------------------------------------------
static inline float fp_maxmin32(float x, float y, int is_min, int num) {
    float32x2_t a = vdup_n_f32(x), b = vdup_n_f32(y);
    return vget_lane_f32(num ? (is_min ? vminnm_f32(a, b) : vmaxnm_f32(a, b)) : (is_min ? vmin_f32(a, b) : vmax_f32(a, b)), 0);
}
static inline double fp_maxmin64(double x, double y, int is_min, int num) {
    float64x1_t a = vdup_n_f64(x), b = vdup_n_f64(y);
    return vget_lane_f64(num ? (is_min ? vminnm_f64(a, b) : vmaxnm_f64(a, b)) : (is_min ? vmin_f64(a, b) : vmax_f64(a, b)), 0);
}

// The arithmetic is written as the instruction: LLVM regards fadd / fmul / fma as commutative and rewrites x - (-y) as x + y, which
// is free for numbers but changes WHICH NaN comes out, or its sign, when an operand is a NaN; the instruction keeps the guest's
// operand order. Not volatile: the compiler still schedules, merges and deletes it like an expression.
#define NX_FP_OP2(name, T, insn, r) \
    static inline T name(T a, T b) { T x; __asm__(insn " %" r "0, %" r "1, %" r "2" : "=w"(x) : "w"(a), "w"(b)); return x; }
NX_FP_OP2(fp_add32, float, "fadd", "s")
NX_FP_OP2(fp_sub32, float, "fsub", "s")
NX_FP_OP2(fp_mul32, float, "fmul", "s")
NX_FP_OP2(fp_div32, float, "fdiv", "s")
NX_FP_OP2(fp_add64, double, "fadd", "d")
NX_FP_OP2(fp_sub64, double, "fsub", "d")
NX_FP_OP2(fp_mul64, double, "fmul", "d")
NX_FP_OP2(fp_div64, double, "fdiv", "d")
static inline float fp_sqrt32(float a) { return __builtin_sqrtf(a); }
static inline double fp_sqrt64(double a) { return __builtin_sqrt(a); }
// FMADD: the addend-first NaN order and the default NaN for a quiet NaN addend with inf * 0 are the instruction's own.
static inline float fp_fma32(float n, float m, float a) {
    float x; __asm__("fmadd %s0, %s1, %s2, %s3" : "=w"(x) : "w"(n), "w"(m), "w"(a)); return x;
}
static inline double fp_fma64(double n, double m, double a) {
    double x; __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(x) : "w"(n), "w"(m), "w"(a)); return x;
}
static inline float fp_d2s(double x) { return (float)x; }       // FCVT quietens a signalling NaN and keeps its top payload bits
static inline double fp_s2d(float x) { return (double)x; }
// FRINTx (mode as in fpexact.h): the instructions quieten a signalling NaN themselves
static inline float fp_frint32(float x, int mode) {
    switch (mode) {
    case 1: return __builtin_roundf(x);
    case 2: return __builtin_floorf(x);
    case 3: return __builtin_ceilf(x);
    case 4: return __builtin_truncf(x);
    default: return __builtin_rintf(x);
    }
}
static inline double fp_frint64(double x, int mode) {
    switch (mode) {
    case 1: return __builtin_round(x);
    case 2: return __builtin_floor(x);
    case 3: return __builtin_ceil(x);
    case 4: return __builtin_trunc(x);
    default: return __builtin_rint(x);
    }
}
// FCVTZS / FCVTZU (ctx.h): NaN to 0, saturation, rounding towards zero
static inline uint32_t f2s32(double v) { uint32_t r; __asm__("fcvtzs %w0, %d1" : "=r"(r) : "w"(v)); return r; }
static inline uint32_t f2u32(double v) { uint32_t r; __asm__("fcvtzu %w0, %d1" : "=r"(r) : "w"(v)); return r; }
static inline uint64_t f2s64(double v) { uint64_t r; __asm__("fcvtzs %x0, %d1" : "=r"(r) : "w"(v)); return r; }
static inline uint64_t f2u64(double v) { uint64_t r; __asm__("fcvtzu %x0, %d1" : "=r"(r) : "w"(v)); return r; }

// ---- FCMP flags (locals.h): the same four results without branches, so clang folds a reader into one FCMP and a branch -----
template <typename T>
NX_FX_INLINE void fx_fcmp(NxF& f, T a, T b) {
    f.N = a < b;
    f.Z = a == b;
    f.C = !(a < b);
    f.V = __builtin_isunordered(a, b);
}

// ---- vectors (neon.h): one host instruction per guest instruction; op, dbl and n are constants at every call site ---------
NX_NEON_INLINE void v_store_n(V128 *d, uint64x2_t r, int dbl, int n) {   // lanes at and above n are cleared, as the portable code does
    if (n * (dbl ? 8 : 4) == 8) r = vcombine_u64(vget_low_u64(r), vdup_n_u64(0));
    else if (n * (dbl ? 8 : 4) == 4) r = vcombine_u64(vand_u64(vget_low_u64(r), vdup_n_u64(0xffffffffull)), vdup_n_u64(0));
    vst1q_u64(d->d, r);
}
#define NX_V_OP2(name, T, insn, arr) \
    NX_NEON_INLINE T name(T a, T b) { T x; __asm__(insn " %0." arr ", %1." arr ", %2." arr : "=w"(x) : "w"(a), "w"(b)); return x; }
#define NX_V_OP3(name, T, insn, arr) \
    NX_NEON_INLINE T name(T acc, T a, T b) { __asm__(insn " %0." arr ", %1." arr ", %2." arr : "+w"(acc) : "w"(a), "w"(b)); return acc; }
NX_V_OP2(v_nadd4s, float32x4_t, "fadd", "4s")
NX_V_OP2(v_nsub4s, float32x4_t, "fsub", "4s")
NX_V_OP2(v_nmul4s, float32x4_t, "fmul", "4s")
NX_V_OP2(v_ndiv4s, float32x4_t, "fdiv", "4s")
NX_V_OP3(v_nmla4s, float32x4_t, "fmla", "4s")
NX_V_OP3(v_nmls4s, float32x4_t, "fmls", "4s")
NX_V_OP2(v_nadd2d, float64x2_t, "fadd", "2d")
NX_V_OP2(v_nsub2d, float64x2_t, "fsub", "2d")
NX_V_OP2(v_nmul2d, float64x2_t, "fmul", "2d")
NX_V_OP2(v_ndiv2d, float64x2_t, "fdiv", "2d")
NX_V_OP3(v_nmla2d, float64x2_t, "fmla", "2d")
NX_V_OP3(v_nmls2d, float64x2_t, "fmls", "2d")
NX_NEON_INLINE void v_f3q(V128 *d, float32x4_t a, float32x4_t b, int op, int dbl, int n) {
    uint64x2_t r;
    if (dbl) {
        float64x2_t x = vreinterpretq_f64_f32(a), y = vreinterpretq_f64_f32(b), acc = vld1q_f64(d->df);
        switch (op) {
        case F_ADD: r = vreinterpretq_u64_f64(v_nadd2d(x, y)); break;
        case F_SUB: r = vreinterpretq_u64_f64(v_nsub2d(x, y)); break;
        case F_MUL: r = vreinterpretq_u64_f64(v_nmul2d(x, y)); break;
        case F_DIV: r = vreinterpretq_u64_f64(v_ndiv2d(x, y)); break;
        case F_MLA: r = vreinterpretq_u64_f64(v_nmla2d(acc, x, y)); break;
        case F_MLS: r = vreinterpretq_u64_f64(v_nmls2d(acc, x, y)); break;
        case F_MAX: r = vreinterpretq_u64_f64(vmaxq_f64(x, y)); break;
        case F_MIN: r = vreinterpretq_u64_f64(vminq_f64(x, y)); break;
        case F_MAXNM: r = vreinterpretq_u64_f64(vmaxnmq_f64(x, y)); break;
        case F_MINNM: r = vreinterpretq_u64_f64(vminnmq_f64(x, y)); break;
        case F_ABD: r = vreinterpretq_u64_f64(vabdq_f64(x, y)); break;
        case F_CMEQ: r = vceqq_f64(x, y); break;
        case F_CMGE: r = vcgeq_f64(x, y); break;
        case F_CMGT: r = vcgtq_f64(x, y); break;
        case F_RECPS: r = vreinterpretq_u64_f64(vrecpsq_f64(x, y)); break;
        case F_RSQRTS: r = vreinterpretq_u64_f64(vrsqrtsq_f64(x, y)); break;
        default: r = vreinterpretq_u64_f64(vpaddq_f64(x, y)); break;           /* F_ADDP */
        }
    } else {
        float32x4_t acc = vld1q_f32(d->f);
        switch (op) {
        case F_ADD: r = vreinterpretq_u64_f32(v_nadd4s(a, b)); break;
        case F_SUB: r = vreinterpretq_u64_f32(v_nsub4s(a, b)); break;
        case F_MUL: r = vreinterpretq_u64_f32(v_nmul4s(a, b)); break;
        case F_DIV: r = vreinterpretq_u64_f32(v_ndiv4s(a, b)); break;
        case F_MLA: r = vreinterpretq_u64_f32(v_nmla4s(acc, a, b)); break;
        case F_MLS: r = vreinterpretq_u64_f32(v_nmls4s(acc, a, b)); break;
        case F_MAX: r = vreinterpretq_u64_f32(vmaxq_f32(a, b)); break;
        case F_MIN: r = vreinterpretq_u64_f32(vminq_f32(a, b)); break;
        case F_MAXNM: r = vreinterpretq_u64_f32(vmaxnmq_f32(a, b)); break;
        case F_MINNM: r = vreinterpretq_u64_f32(vminnmq_f32(a, b)); break;
        case F_ABD: r = vreinterpretq_u64_f32(vabdq_f32(a, b)); break;
        case F_CMEQ: r = vreinterpretq_u64_u32(vceqq_f32(a, b)); break;
        case F_CMGE: r = vreinterpretq_u64_u32(vcgeq_f32(a, b)); break;
        case F_CMGT: r = vreinterpretq_u64_u32(vcgtq_f32(a, b)); break;
        case F_RECPS: r = vreinterpretq_u64_f32(vrecpsq_f32(a, b)); break;
        case F_RSQRTS: r = vreinterpretq_u64_f32(vrsqrtsq_f32(a, b)); break;
        default:                                                                 /* F_ADDP: pairs of n, then pairs of m */
            r = n == 4 ? vreinterpretq_u64_f32(vpaddq_f32(a, b))
                       : vcombine_u64(vreinterpret_u64_f32(vpadd_f32(vget_low_f32(a), vget_low_f32(b))), vdup_n_u64(0));
            break;
        }
    }
    v_store_n(d, r, dbl, n);
}
NX_NEON_INLINE void v_f3v(V128 *d, const V128 *pn, const V128 *pm, int op, int dbl, int n) {
    v_f3q(d, vld1q_f32(pn->f), vld1q_f32(pm->f), op, dbl, n);
}
// FMUL / FMLA / FMLS by element are single instructions (the element index is an immediate); idx is a literal at every call.
#define NX_V_ELEM(insn, arr, earr, i) __asm__(insn " %0." arr ", %1." arr ", %2." earr "[" #i "]" : "+w"(x) : "w"(a), "w"(b))
#define NX_V_ELEM_CASES(insn) \
    if (dbl) switch (idx) { case 0: NX_V_ELEM(insn, "2d", "d", 0); break; default: NX_V_ELEM(insn, "2d", "d", 1); break; } \
    else switch (idx) { case 0: NX_V_ELEM(insn, "4s", "s", 0); break; case 1: NX_V_ELEM(insn, "4s", "s", 1); break; \
                        case 2: NX_V_ELEM(insn, "4s", "s", 2); break; default: NX_V_ELEM(insn, "4s", "s", 3); break; }
NX_NEON_INLINE void v_f3e(V128 *d, const V128 *pn, const V128 *pm, int idx, int op, int dbl, int n) {
    float32x4_t a = vld1q_f32(pn->f), b = vld1q_f32(pm->f), x;
    switch (op) {
    case F_MUL:
        if (dbl) switch (idx) {
            case 0: __asm__("fmul %0.2d, %1.2d, %2.d[0]" : "=w"(x) : "w"(a), "w"(b)); break;
            default: __asm__("fmul %0.2d, %1.2d, %2.d[1]" : "=w"(x) : "w"(a), "w"(b)); break; }
        else switch (idx) {
            case 0: __asm__("fmul %0.4s, %1.4s, %2.s[0]" : "=w"(x) : "w"(a), "w"(b)); break;
            case 1: __asm__("fmul %0.4s, %1.4s, %2.s[1]" : "=w"(x) : "w"(a), "w"(b)); break;
            case 2: __asm__("fmul %0.4s, %1.4s, %2.s[2]" : "=w"(x) : "w"(a), "w"(b)); break;
            default: __asm__("fmul %0.4s, %1.4s, %2.s[3]" : "=w"(x) : "w"(a), "w"(b)); break; }
        break;
    case F_MLA: x = vld1q_f32(d->f); NX_V_ELEM_CASES("fmla") break;
    case F_MLS: x = vld1q_f32(d->f); NX_V_ELEM_CASES("fmls") break;
    default:
        v_f3q(d, a, dbl ? vreinterpretq_f32_f64(vdupq_n_f64(pm->df[idx])) : vdupq_n_f32(pm->f[idx]), op, dbl, n);
        return;
    }
    v_store_n(d, vreinterpretq_u64_f32(x), dbl, n);
}
NX_NEON_INLINE void v_f1v(V128 *d, const V128 *pn, int op, int dbl, int n) {
    uint64x2_t r;
    if (dbl) {
        float64x2_t x = vld1q_f64(pn->df);
        switch (op) {
        case F1_ABS: r = vreinterpretq_u64_f64(vabsq_f64(x)); break;
        case F1_NEG: r = vreinterpretq_u64_f64(vnegq_f64(x)); break;
        case F1_SQRT: r = vreinterpretq_u64_f64(vsqrtq_f64(x)); break;
        case F1_RSQRTE: r = vreinterpretq_u64_f64(vrsqrteq_f64(x)); break;
        case F1_RECPE: r = vreinterpretq_u64_f64(vrecpeq_f64(x)); break;
        case F1_SCVTF: r = vreinterpretq_u64_f64(vcvtq_f64_s64(vld1q_s64(pn->s64))); break;
        case F1_UCVTF: r = vreinterpretq_u64_f64(vcvtq_f64_u64(vld1q_u64(pn->d))); break;
        case F1_CVTZS: r = vreinterpretq_u64_s64(vcvtq_s64_f64(x)); break;
        case F1_CVTZU: r = vcvtq_u64_f64(x); break;
        case F1_CMEQ0: r = vceqzq_f64(x); break;
        case F1_CMGT0: r = vcgtzq_f64(x); break;
        case F1_CMGE0: r = vcgezq_f64(x); break;
        case F1_CMLT0: r = vcltzq_f64(x); break;
        default: r = vclezq_f64(x); break;                                       /* F1_CMLE0 */
        }
    } else {
        float32x4_t x = vld1q_f32(pn->f);
        switch (op) {
        case F1_ABS: r = vreinterpretq_u64_f32(vabsq_f32(x)); break;
        case F1_NEG: r = vreinterpretq_u64_f32(vnegq_f32(x)); break;
        case F1_SQRT: r = vreinterpretq_u64_f32(vsqrtq_f32(x)); break;
        case F1_RSQRTE: r = vreinterpretq_u64_f32(vrsqrteq_f32(x)); break;
        case F1_RECPE: r = vreinterpretq_u64_f32(vrecpeq_f32(x)); break;
        case F1_SCVTF: r = vreinterpretq_u64_f32(vcvtq_f32_s32(vld1q_s32(pn->s32))); break;
        case F1_UCVTF: r = vreinterpretq_u64_f32(vcvtq_f32_u32(vld1q_u32(pn->w))); break;
        case F1_CVTZS: r = vreinterpretq_u64_s32(vcvtq_s32_f32(x)); break;
        case F1_CVTZU: r = vreinterpretq_u64_u32(vcvtq_u32_f32(x)); break;
        case F1_CMEQ0: r = vreinterpretq_u64_u32(vceqzq_f32(x)); break;
        case F1_CMGT0: r = vreinterpretq_u64_u32(vcgtzq_f32(x)); break;
        case F1_CMGE0: r = vreinterpretq_u64_u32(vcgezq_f32(x)); break;
        case F1_CMLT0: r = vreinterpretq_u64_u32(vcltzq_f32(x)); break;
        default: r = vreinterpretq_u64_u32(vclezq_f32(x)); break;               /* F1_CMLE0 */
        }
    }
    v_store_n(d, r, dbl, n);
}
