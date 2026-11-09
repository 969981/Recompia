// SPDX-License-Identifier: GPL-3.0-or-later
// FNV-1a 64 step of the NVN back end. Callers keep their own seed and feed (bytes, 32-bit or 64-bit words). Some of these hashes are
// written to disk (prewarm records, compute_layout_key, module ids, tex_patch rectangles): a caller's seed, feed and order never change.
#pragma once

#include <cstdint>

namespace nxrt::nvn::be {

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;          // the FNV-1a 64 offset basis (14695981039346656037)
constexpr uint64_t kFnvBasisShort = 1469598103934665603ull;    // the basis without its last digit, as most hashes here were written
constexpr uint64_t kFnvPrime = 0x100000001b3ull;               // 1099511628211

// v is xored in whole (zero-extended), then the product with the prime (mod 2^64).
constexpr uint64_t fnv_step(uint64_t h, uint64_t v) { return (h ^ v) * kFnvPrime; }

}  // namespace nxrt::nvn::be
