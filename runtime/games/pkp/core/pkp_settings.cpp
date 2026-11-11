// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the getters and setters of the [pkp] keys (pkp_settings.h); the table is pkp_settings_table.cpp.
#include "core/pkp_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/ini_edit.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::settings {

namespace {

constexpr const char* kTag = "pkp";

std::atomic<uint64_t> g_misread{0};   // one bit per key used through the wrong getter or setter, logged once

bool typed(const Spec& sp, Type t) {
    if (sp.type == t) return true;
    const uint64_t bit = uint64_t{1} << static_cast<unsigned>(sp.key);
    if (!(g_misread.fetch_or(bit, std::memory_order_relaxed) & bit)) NXLOG_W(kTag, "settings: [pkp] %s used as the wrong type", sp.name);
    return false;
}

double clamp(const Spec& sp, double v) { return sp.lo < sp.hi ? std::clamp(v, sp.lo, sp.hi) : v; }

bool is_choice(const Spec& sp, std::string_view v) {
    if (!*sp.choices) return true;
    for (std::string_view rest = sp.choices; !rest.empty();) {
        const size_t sp_at = rest.find(' ');
        if (rest.substr(0, sp_at) == v) return true;
        rest = sp_at == std::string_view::npos ? std::string_view() : rest.substr(sp_at + 1);
    }
    return false;
}

// In force at once, then saved to the game ini, relative to the game folder (the working folder of an installed game, pkp_main.cpp). A
// run started with --ini or NXRT_INI reads another file: the value is then saved to the game ini all the same (when it exists).
bool store(const Spec& sp, const std::string& text) {
    config_set(kSection, sp.name, text.c_str());
    const char* ini = game::info().game_ini;
    std::string err;
    if (!*ini) err = "the game names no ini file";
    else if (ini_file_set_value(ini, kSection, sp.name, text, &err)) return true;
    NXLOG_W(kTag, "settings: [pkp] %s = %s is in force but not saved: %s", sp.name, text.c_str(), err.c_str());
    return false;
}

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

bool set_bool(Key k, bool v) {
    const Spec& sp = spec(k);
    return typed(sp, Type::Bool) && store(sp, v ? "1" : "0");
}

bool set_int(Key k, int64_t v) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::Int)) return false;
    if (sp.lo < sp.hi) v = std::clamp(v, static_cast<int64_t>(sp.lo), static_cast<int64_t>(sp.hi));
    return store(sp, std::to_string(v));
}

bool set_double(Key k, double v) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::Double)) return false;
    // Rounded to 1e-9: a row's lo + n * step (-2 + 20 * 0.1 = 4.4e-16) is written as the number it means (0), never -0 or an exponent.
    double r = std::round(clamp(sp, v) * 1e9) / 1e9;
    if (r == 0) r = 0;
    char text[32];
    std::snprintf(text, sizeof text, "%.10g", r);
    return store(sp, text);
}

bool set_str(Key k, const std::string& v) {
    const Spec& sp = spec(k);
    if (!typed(sp, Type::String)) return false;
    if (!is_choice(sp, v)) {
        NXLOG_W(kTag, "settings: [pkp] %s takes %s, not %s", sp.name, sp.choices, v.c_str());
        return false;
    }
    return store(sp, v);
}

}  // namespace nxrt::games::pkp::settings
