// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the getters of the [pkp] keys (pkp_settings.h); the table is pkp_settings_table.cpp.
#include "pkp_settings.h"

#include <algorithm>
#include <atomic>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::settings {

namespace {

constexpr const char* kTag = "pkp";

std::atomic<uint64_t> g_misread{0};   // one bit per key read through the wrong getter, logged once

bool typed(const Spec& sp, Type t) {
    if (sp.type == t) return true;
    const uint64_t bit = uint64_t{1} << static_cast<unsigned>(sp.key);
    if (!(g_misread.fetch_or(bit, std::memory_order_relaxed) & bit)) NXLOG_W(kTag, "settings: [pkp] %s read as the wrong type", sp.name);
    return false;
}

double clamp(const Spec& sp, double v) { return sp.lo < sp.hi ? std::clamp(v, sp.lo, sp.hi) : v; }

}  // namespace

static_assert(static_cast<size_t>(Key::Count) <= 64, "g_misread holds one bit per key");

bool get_bool(Key k) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::Bool)) return sp.def != 0;
    return config_get_bool(kSection, sp.name, sp.def != 0);
}

int64_t get_int(Key k) {
    const Spec& sp = spec(k);
    const int64_t def = static_cast<int64_t>(sp.def);
    if (!typed(sp, Type::Int)) return def;
    const int64_t v = config_get_int(kSection, sp.name, def);
    return sp.lo < sp.hi ? std::clamp(v, static_cast<int64_t>(sp.lo), static_cast<int64_t>(sp.hi)) : v;
}

double get_double(Key k) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::Double)) return sp.def;
    return clamp(sp, config_get_double(kSection, sp.name, sp.def));
}

std::string get_str(Key k) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::String)) return sp.def_str;
    return config_get_str(kSection, sp.name, sp.def_str);
}

bool is_set(Key k) { return config_has(kSection, spec(k).name); }

}  // namespace nxrt::games::pkp::settings
