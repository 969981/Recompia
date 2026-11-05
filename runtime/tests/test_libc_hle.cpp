// SPDX-License-Identifier: GPL-3.0-or-later
// The SDK's string and memory functions as host code (src/core/libc_hle.cpp, [hle] libc): registered as leaf handlers with the C results.
#include <cstdint>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "test_framework.h"
NXTEST_GROUP(libc_hle)

namespace {
uint64_t call(const char* name, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0) {
    nxrt::HleFn fn = nxrt::hle_leaf_fn(nxrt::hle_address(name));
    if (!fn) return ~0ull;
    ::Ctx c{};
    c.x[0] = a0;
    c.x[1] = a1;
    c.x[2] = a2;
    fn(&c);
    return c.x[0];
}
uint64_t u(const void* p) { return reinterpret_cast<uint64_t>(p); }
}  // namespace

NXTEST(libc_functions_match_the_crt) {
    nxrt::install_libc_hle();
    char a[32] = "hello world", b[32] = "hello there";
    CHECK_EQ(call("strlen", u(a)), 11ull);
    CHECK_EQ(call("strnlen", u(a), 5), 5ull);
    CHECK((int32_t)call("strcmp", u(a), u(b)) > 0);
    CHECK_EQ((int32_t)call("strncmp", u(a), u(b), 6), 0);
    CHECK_EQ((int32_t)call("memcmp", u(a), u(a), 11), 0);
    CHECK_EQ(call("strchr", u(a), 'w'), u(a + 6));
    CHECK_EQ(call("strrchr", u(a), 'o'), u(a + 7));
    CHECK_EQ(call("strstr", u(a), u("wor")), u(a + 6));
    CHECK_EQ(call("memchr", u(a), 'z', 11), 0ull);
    char buf[32];
    CHECK_EQ(call("memset", u(buf), 0x5A, 8), u(buf));
    CHECK_EQ((int)buf[7], 0x5A);
    call("memcpy", u(buf), u(a), 12);
    CHECK_EQ(std::strcmp(buf, a), 0);
    call("memmove", u(buf + 2), u(buf), 6);                  // overlapping copy is not torn
    CHECK_EQ(std::memcmp(buf + 2, "hello ", 6), 0);
    call("strcpy", u(buf), u(b));
    CHECK_EQ(std::strcmp(buf, b), 0);
}
