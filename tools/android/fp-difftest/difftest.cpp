// SPDX-License-Identifier: GPL-3.0-or-later
// Differential test of the AArch64 fast paths of the lifted code's FP / Advanced SIMD helpers (recomp/include/nxrecomp:
// fpexact.h, neon.h, locals.h; switch NX_NATIVE_FP). Runs the portable helpers (gen) and the native ones (nat) on the same
// inputs, random bit patterns mixed with edge values (signed zeros, denormals, infinities, quiet and signalling NaNs with
// payloads, saturation and rounding boundaries), and compares every result bit for bit. Must run on an AArch64 machine.
//   difftest [iterations per helper, default 400000] [seed]   exit 0 = no mismatch
//   difftest --bench [iterations]                             speed of v_f3v fmul.4s and v_f3e fmla.4s on random inputs, portable vs native
#include <chrono>
#include <thread>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "impl.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static const uint32_t kEdge32[] = {
    0x00000000, 0x00000001, 0x007fffff, 0x00800000, 0x00400000, 0x3f800000, 0x40000000, 0x3f000000, 0x3fc00000, 0x40200000,
    0x3effffff, 0x7f7fffff, 0x7f800000, 0x7fc00000, 0x7fc00001, 0x7fffffff, 0x7f800001, 0x7fa00000, 0x7fbfffff, 0x4effffff,
    0x4f000000, 0x4f7fffff, 0x4f800000, 0x5effffff, 0x5f000000, 0x5f800000, 0x34000000, 0x33800000, 0x00000003, 0x40400000};
static const uint64_t kEdge64[] = {
    0, 1, 0x000fffffffffffffull, 0x0010000000000000ull, 0x3ff0000000000000ull, 0x4000000000000000ull, 0x3fe0000000000000ull,
    0x3ff8000000000000ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull, 0x7ff8000000000000ull, 0x7ff8000000000001ull,
    0x7fffffffffffffffull, 0x7ff0000000000001ull, 0x7ff4000000000000ull, 0x7ff7ffffffffffffull, 0x41dfffffffc00000ull,
    0x41e0000000000000ull, 0x41efffffffe00000ull, 0x41f0000000000000ull, 0x43dfffffffffffffull, 0x43e0000000000000ull,
    0x43f0000000000000ull, 0x36a0000000000000ull, 0x380fffffe0000000ull, 0x47efffffe0000000ull, 0x47efffff00000000ull,
    0x7ff0000020000000ull, 0x7ff00000ffffffffull, 0x3ca0000000000000ull};

static uint32_t val32() {
    uint64_t r = rnd();
    uint32_t sign = uint32_t(r >> 63) << 31, bits = uint32_t(r >> 8);
    switch (r & 3) {
    case 0: return kEdge32[(r >> 2) % (sizeof kEdge32 / 4)] | sign;
    case 1: return bits;
    case 2: return sign | (uint32_t(100 + (r >> 40) % 56) << 23) | (bits & 0x7fffff);       // ordinary magnitudes
    default: return sign | ((r >> 2) & 1 ? 0x7f800000u : 0u) | (bits & 0x7fffff);           // NaN / inf / denormal / zero
    }
}
static uint64_t val64() {
    uint64_t r = rnd(), bits = rnd();
    uint64_t sign = (r >> 63) << 63;
    switch (r & 3) {
    case 0: return kEdge64[(r >> 2) % (sizeof kEdge64 / 8)] | sign;
    case 1: return bits;
    case 2: return sign | (uint64_t(1000 + (r >> 40) % 47) << 52) | (bits & 0xfffffffffffffull);
    default: return sign | ((r >> 2) & 1 ? 0x7ff0000000000000ull : 0) | (bits & 0xfffffffffffffull);
    }
}
static void fill(V128 &v, bool dbl) {
    if (dbl) for (auto &x : v.d) x = val64(); else for (auto &x : v.w) x = val32();
}
static void share(V128 &a, const V128 &b) {   // some lanes equal (or the other zero), for compares and x - x
    for (int i = 0; i < 4; i++) if ((rnd() & 7) == 0) a.w[i] = b.w[i] ^ ((rnd() & 1) ? 0x80000000u : 0u);
}

static long g_bad = 0;
static int g_shown = 0;
static void report(const char *what, int op, int sh, const V128 &a, const V128 &b, const V128 &d0, const V128 &g, const V128 &n) {
    g_bad++;
    if (g_shown++ < 40)
        printf("MISMATCH %s op %d shape %d\n  a %016" PRIx64 "%016" PRIx64 " b %016" PRIx64 "%016" PRIx64 " d %016" PRIx64
               "%016" PRIx64 "\n  gen %016" PRIx64 "%016" PRIx64 "\n  nat %016" PRIx64 "%016" PRIx64 "\n",
               what, op, sh, a.d[1], a.d[0], b.d[1], b.d[0], d0.d[1], d0.d[0], g.d[1], g.d[0], n.d[1], n.d[0]);
}

static void vec(const char *what, const VecFn *G, const VecFn *N, int nops, long iters) {
    for (int op = 0; op < nops; op++)
        for (int sh = 0; sh < 5; sh++) {
            if (!strcmp(what, "v_f3v") && op == 16 && sh == 0) continue;   // FADDP of one single lane: never emitted, no meaning
            long before = g_bad;
            for (long k = 0; k < iters; k++) {
                bool dbl = sh >= 3;
                V128 a, b, d0;
                fill(a, dbl); fill(b, dbl); fill(d0, dbl);
                if (rnd() & 1) share(b, a);
                int idx = int(rnd() & 3);
                V128 g = d0, n = d0;
                G[op * 5 + sh](&g, &a, &b, idx);
                N[op * 5 + sh](&n, &a, &b, idx);
                if (memcmp(&g, &n, 16)) report(what, op, sh, a, b, d0, g, n);
            }
            if (g_bad != before) printf("%s op %d shape %d: %ld mismatches\n", what, op, sh, g_bad - before);
        }
}

template <typename F, typename U, typename Fn>
static void scal(const char *what, int op, Fn g, Fn n, U (*val)(), long iters) {
    long before = g_bad;
    for (long k = 0; k < iters; k++) {
        U ua = val(), ub = (rnd() & 7) == 0 ? ua : val();
        F a, b; memcpy(&a, &ua, sizeof a); memcpy(&b, &ub, sizeof b);
        F rg = g(a, b), rn = n(a, b);
        if (memcmp(&rg, &rn, sizeof rg)) {
            V128 A{}, B{}, Gv{}, Nv{}, Z{};
            memcpy(&A, &a, sizeof a); memcpy(&B, &b, sizeof b); memcpy(&Gv, &rg, sizeof rg); memcpy(&Nv, &rn, sizeof rn);
            report(what, op, 0, A, B, Z, Gv, Nv);
        }
    }
    if (g_bad != before) printf("%s op %d: %ld mismatches\n", what, op, g_bad - before);
}

static double bench(VecFn f, long iters) {
    V128 in[64], out[64];
    for (auto &v : in) for (int i = 0; i < 4; i++) v.f[i] = float(int(rnd() % 2001) - 1000) / 64.0f;
    auto t0 = std::chrono::steady_clock::now();
    for (long k = 0; k < iters; k++) { int i = int(k & 63); out[i] = in[i]; f(&out[i], &in[(i + 1) & 63], &in[(i + 7) & 63], 0); }
    auto t1 = std::chrono::steady_clock::now();
    volatile float sink = out[5].f[1]; (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(iters);
}

int main(int argc, char **argv) {
    uint64_t fpcr = 0;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
#endif
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        long it = argc > 2 ? atol(argv[2]) : 20000000;
        const Impl &G = gen::impl, &N = nat::impl;
        printf("fpcr %#" PRIx64 "\n", fpcr);
        for (int rep = 0; rep < 2; rep++) {
            double mg = bench(G.f3v[2 * 5 + 2], it), mn = bench(N.f3v[2 * 5 + 2], it);       // F_MUL single n = 4
            double ag = bench(G.f3e[4 * 5 + 2], it), an = bench(N.f3e[4 * 5 + 2], it);       // F_MLA by element n = 4
            printf("v_f3v fmul.4s: portable %.2f ns, native %.2f ns, ratio %.2fx\n", mg, mn, mg / mn);
            printf("v_f3e fmla.4s: portable %.2f ns, native %.2f ns, ratio %.2fx\n", ag, an, ag / an);
        }
        return 0;
    }
    long it = argc > 1 ? atol(argv[1]) : 400000;
    if (argc > 2) rs = strtoull(argv[2], nullptr, 0) | 1;
    printf("fpcr %#" PRIx64 " (must be 0), %ld per helper\n", fpcr, it);
    const Impl &G = gen::impl, &N = nat::impl;
    vec("v_f3v", G.f3v.data(), N.f3v.data(), 17, it);
    vec("v_f3e", G.f3e.data(), N.f3e.data(), 16, it);
    vec("v_f1v", G.f1v.data(), N.f1v.data(), 14, it);
    for (int op = 0; op < 10; op++) scal<float, uint32_t>("s32", op, G.s32[op], N.s32[op], val32, it);
    for (int op = 0; op < 10; op++) scal<double, uint64_t>("s64", op, G.s64[op], N.s64[op], val64, it);
    for (int op = 0; op < 5; op++) scal<float, uint32_t>("frint32", op, G.frint32[op], N.frint32[op], val32, it);
    for (int op = 0; op < 5; op++) scal<double, uint64_t>("frint64", op, G.frint64[op], N.frint64[op], val64, it);
    for (int op = 0; op < 4; op++) {   // f2s32 ... f2u64 on doubles, and on singles widened (the lifter's (double)s form)
        long b0 = g_bad;
        for (long k = 0; k < it; k++) {
            uint64_t u = val64(); double d; memcpy(&d, &u, 8);
            if (k & 1) { uint32_t w = val32(); float f; memcpy(&f, &w, 4); d = double(f); }
            uint64_t rg = G.cvt[op](d), rn = N.cvt[op](d);
            if (rg != rn) { V128 A{}, B{}, Z{}, Gv{}, Nv{}; memcpy(&A, &d, 8); Gv.d[0] = rg; Nv.d[0] = rn; report("cvt", op, 0, A, B, Z, Gv, Nv); }
        }
        if (g_bad != b0) printf("cvt op %d: %ld mismatches\n", op, g_bad - b0);
    }
    long before = g_bad;
    for (long k = 0; k < it * 4; k++) {
        uint32_t u[3] = {val32(), val32(), val32()};
        float x[3]; memcpy(x, u, 12);
        if ((rnd() & 3) == 0) x[(rnd() >> 3) % 3] = 0.0f;            // inf * 0 with a NaN addend needs zeros
        float rg = G.fma32(x[0], x[1], x[2]), rn = N.fma32(x[0], x[1], x[2]);
        if (memcmp(&rg, &rn, 4)) { V128 A{}, B{}, D{}, Gv{}, Nv{}; memcpy(&A, x, 8); memcpy(&D, &x[2], 4); memcpy(&Gv, &rg, 4); memcpy(&Nv, &rn, 4); report("fma32", 0, 0, A, B, D, Gv, Nv); }
        uint64_t v[3] = {val64(), val64(), val64()};
        double y[3]; memcpy(y, v, 24);
        if ((rnd() & 3) == 0) y[(rnd() >> 3) % 3] = 0.0;
        double qg = G.fma64(y[0], y[1], y[2]), qn = N.fma64(y[0], y[1], y[2]);
        if (memcmp(&qg, &qn, 8)) { V128 A{}, B{}, D{}, Gv{}, Nv{}; memcpy(&A, y, 8); memcpy(&B, &y[1], 8); memcpy(&D, &y[2], 8); memcpy(&Gv, &qg, 8); memcpy(&Nv, &qn, 8); report("fma64", 0, 0, A, B, D, Gv, Nv); }
    }
    if (g_bad != before) printf("fma: %ld mismatches\n", g_bad - before);
    auto cmp = [](const char *w, auto g, auto n, auto val, auto tag) {
        using F = decltype(tag);
        long b0 = g_bad;
        for (long k = 0; k < 400000; k++) {
            auto ua = val(), ub = (rnd() & 3) == 0 ? ua ^ ((rnd() & 1) ? (decltype(ua))1 << (sizeof(F) * 8 - 1) : 0) : val();
            F a, b; memcpy(&a, &ua, sizeof a); memcpy(&b, &ub, sizeof b);
            uint64_t rg = g(a, b), rn = n(a, b);
            if (rg != rn) { V128 A{}, B{}, Z{}, Gv{}, Nv{}; memcpy(&A, &a, sizeof a); memcpy(&B, &b, sizeof b); Gv.d[0] = rg; Nv.d[0] = rn; report(w, 0, 0, A, B, Z, Gv, Nv); }
        }
        if (g_bad != b0) printf("%s: %ld mismatches\n", w, g_bad - b0);
    };
    cmp("fcmp32", G.fcmp32, N.fcmp32, val32, 0.0f);
    cmp("fcmp64", G.fcmp64, N.fcmp64, val64, 0.0);
    cmp("fcond32", G.fcond32, N.fcond32, val32, 0.0f);
    cmp("fccop32 native", G.fcond32, N.fccop32, val32, 0.0f);
    // compare-and-swap of the native build (Android forced include) against the compiler's builtin in this unit: memory, success
    // and the returned value, with expected == desired often
    {
        long b0 = g_bad;
        for (long k = 0; k < it; k++) {
            uint64_t m = rnd(), e = (rnd() & 3) == 0 ? m : rnd(), d = (rnd() & 1) ? e : rnd();
            if ((rnd() & 7) == 0) m = e = d = 0;
#define NX_CAS_CASE(F, T) { T mg = T(m), eg = T(e), mn = T(m), en = T(e); \
            bool sg = __atomic_compare_exchange_n(&mg, &eg, T(d), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST), sn = N.F(&mn, &en, T(d)); \
            if (sg != sn || mg != mn || eg != en) { V128 A{}, B{}, Z{}, Gv{}, Nv{}; A.d[0] = m; A.d[1] = e; B.d[0] = d; \
                Gv.d[0] = mg; Gv.d[1] = uint64_t(eg) | (uint64_t(sg) << 63); Nv.d[0] = mn; Nv.d[1] = uint64_t(en) | (uint64_t(sn) << 63); report(#F, 0, 0, A, B, Z, Gv, Nv); } }
            NX_CAS_CASE(cas8, uint8_t) NX_CAS_CASE(cas16, uint16_t) NX_CAS_CASE(cas32, uint32_t) NX_CAS_CASE(cas64, uint64_t)
#undef NX_CAS_CASE
        }
        if (g_bad != b0) printf("cas: %ld mismatches\n", g_bad - b0);
    }
    // store buffering with the CAS read (expected == desired == 0): seq_cst forbids both threads reading 0; message passing: the data
    // written before a flag set by CAS must be seen after the flag is read by CAS
    {
        long sb_bad = 0, mp_bad = 0;
        for (int round = 0; round < 20000; round++) {
            alignas(64) static uint32_t x, y, data, flag;
            uint32_t r1 = 9, r2 = 9;
            x = y = data = flag = 0;
            std::thread t1([&] { __atomic_store_n(&x, 1, __ATOMIC_RELAXED); uint32_t e = 0; N.cas32(&y, &e, 0); r1 = e;
                                 __atomic_store_n(&data, 42, __ATOMIC_RELAXED); uint32_t f = 0; N.cas32(&flag, &f, 1); });
            std::thread t2([&] { __atomic_store_n(&y, 1, __ATOMIC_RELAXED); uint32_t e = 0; N.cas32(&x, &e, 0); r2 = e;
                                 uint32_t f; do { f = 0; N.cas32(&flag, &f, 0); } while (f == 0);
                                 if (__atomic_load_n(&data, __ATOMIC_RELAXED) != 42) mp_bad++; });
            t1.join(); t2.join();
            if (r1 == 0 && r2 == 0) sb_bad++;
        }
        printf("cas litmus: store buffering %ld forbidden outcomes, message passing %ld stale reads (20000 rounds each)\n", sb_bad, mp_bad);
        g_bad += sb_bad + mp_bad;
    }
    printf("%s: %ld mismatches\n", g_bad ? "FAIL" : "PASS", g_bad);
    return g_bad ? 1 : 0;
}
