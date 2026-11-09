// SPDX-License-Identifier: GPL-3.0-or-later
// A gauge (include/nxrt/gauge.h) registered when a namespace-scope static is built and removed when it is destroyed, so a callback never
// outlives the state it reads. The kernel, loader, mem, os, media and core directories each carry their own copy of this struct; one
// runtime-wide definition (include/nxrt/gauge.h) can replace all of them, this one included.
#pragma once
#include <cstdint>
#include <functional>
#include <utility>

#include "nxrt/gauge.h"

namespace nxrt::services {

struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};

}  // namespace nxrt::services
