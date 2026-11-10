// SPDX-License-Identifier: GPL-3.0-or-later
// Random bytes for the guest and the runtime: the process entropy seed (GetInfo RandomEntropy), csrng's GenerateRandomBytes, generated
// save ids. By default the OS generator (BCryptGenRandom); a seeded source (the test tooling's deterministic mode) replaces it through
// set_source so a replayed run sees the same numbers.
#pragma once
#include <cstddef>

namespace nxrt::entropy {

// Fills p with n random bytes. false: the OS generator failed; the bytes are then a weak stream (host counter based) and the failure is logged.
bool fill(void* p, size_t n);

struct Source {
    void (*fill)(void* p, size_t n);             // must be set
};
// Installs `s` (null: the OS generator again). `s` must stay valid while installed.
void set_source(const Source* s);
bool seeded();                                   // a source is installed

}  // namespace nxrt::entropy
