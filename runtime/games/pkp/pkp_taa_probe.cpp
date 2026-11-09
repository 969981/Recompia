// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, test tool: log-only entry hooks on the engine's temporal AA pass slot setups (offsets in `main` of the 2.0.1 update, base
// 0x7100000000; the passes are described in pkp_taa.cpp). Each logs once when reached, to show whether the pass exists in a run. They change
// nothing. Built only with NXRT_TESTTOOLS (runtime/games/pkp/CMakeLists.txt); a release package has no such hooks.
//   0x7101e53748  TemporalAA3 slot setup (x0 = the pass): its switch byte and the colour / depth / output slot ids
//   0x7101e39b64  TemporalAA (v1) slot setup
//   0x7101e577ac  KTSS slot setup
#include "pkp_taa_probe.h"
#include "pkp_guest.h"

#include <atomic>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kTaa3Setup = 0x1e53748;    // entry: TemporalAA3 pass slot setup (x0 = pass)
constexpr uint64_t kTaa1Setup = 0x1e39b64;    // entry: TemporalAA (v1) pass slot setup
constexpr uint64_t kKtssSetup = 0x1e577ac;    // entry: KTSS pass slot setup
constexpr uint64_t kPassSwitch = 0xc, kPassSlots = 79 * 4, kPassSize = 0x168;

struct State {
    bool installed = false;
    std::atomic<bool> taa3_logged{false}, taa1_logged{false}, ktss_logged{false};
};
State& st() {
    static State s;
    return s;
}

using guest::rd;

int hook_taa3_setup(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t p = c->x[0];
    if (p && mem::is_mapped(p, kPassSize) && !s.taa3_logged.exchange(true))
        NXLOG_I(kTag, "taa: TemporalAA3 pass exists (%llx): switch byte %u, colour / depth / output slot ids %d %d %d", static_cast<unsigned long long>(p),
                rd<uint8_t>(p + kPassSwitch), rd<int32_t>(p + kPassSlots), rd<int32_t>(p + kPassSlots + 4), rd<int32_t>(p + kPassSlots + 8));
    return 0;
}

int hook_setup_once(::Ctx* c, uint64_t, void* user) {
    State& s = st();
    auto* flag = static_cast<std::atomic<bool>*>(user);
    if (!flag->exchange(true))
        NXLOG_I(kTag, "taa: %s pass slot setup called (pass %llx)", flag == &s.taa1_logged ? "TemporalAA (v1)" : "KTSS", static_cast<unsigned long long>(c->x[0]));
    return 0;
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.installed) return;
    in.hooks.add_entry(in.base + kTaa3Setup, &hook_taa3_setup);
    in.hooks.add_entry(in.base + kTaa1Setup, &hook_setup_once, &s.taa1_logged);
    in.hooks.add_entry(in.base + kKtssSetup, &hook_setup_once, &s.ktss_logged);
    s.installed = true;
}

void remove() { st().installed = false; }   // the registry has removed the hooks

}  // namespace

const Feature kTaaProbeFeature = {.name = "taa_probe", .install = &install, .remove = &remove};

}  // namespace nxrt::games::pkp
