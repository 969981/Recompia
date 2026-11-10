// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings file reader, one `key = value` of a fixed section at a time (apply_fixed). The keys and their values are the
// settings table's (play_config_table.cpp); [padmap] and [switch] have one key per pad button or action and are read here. The file format
// is described in play_types.h.
#include <string>

#include "nxrt/game.h"
#include "play_config_internal.h"

namespace nxrt::play {
namespace cfg {

namespace {

// [padmap] <pad button> = default | none | Switch buttons joined by '+'. false with *known cleared for a name that is no pad button.
bool apply_padmap(const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg) {
    PadSrc src;
    if (!parse_pad_src(k, &src)) {   // unknown pad button name: a warning
        *known = false;
        return true;
    }
    uint64_t& dst = s->pad.remap[static_cast<int>(src)];
    if (lower(v) == "default") {
        dst = kPadDefault;
        return true;
    }
    Output o;
    std::string perr;
    if (!parse_output(v, &o, &perr)) {
        *msg = perr;
        return false;
    }
    if (o.lx != 0 || o.ly != 0 || o.rx != 0 || o.ry != 0) {
        *msg = k + " can press Switch buttons only (no stick directions), got '" + v + "'";
        return false;
    }
    dst = o.buttons;
    return true;
}

// [switch] <action> = <output>. An action of an older file that no longer exists is skipped without a warning.
bool apply_switch(const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg) {
    Act a = Act::None;
    if (retired_act(k)) return true;
    if (!parse_act(k, &a) || a == Act::None) {   // unknown action name: a warning
        *known = false;
        return true;
    }
    Output o;
    std::string perr;
    if (!parse_output(v, &o, &perr)) {
        *msg = perr;
        return false;
    }
    s->out[static_cast<int>(a)] = o;
    return true;
}

}  // namespace

bool apply_fixed(const SettingSection& sec, const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg, int* row) {
    *known = true;
    *row = -1;
    switch (sec.kind) {
        case SectionKind::PadMap: return apply_padmap(k, v, s, known, msg);
        case SectionKind::Switch: return apply_switch(k, v, s, known, msg);
        case SectionKind::Rows: break;
    }
    const int i = find_setting(sec.name, k);
    if (i < 0) {
        *known = false;
        return true;
    }
    if (!setting_read(settings_table()[i], k, v, s, msg)) return false;
    *row = i;
    return true;
}

}  // namespace cfg
}  // namespace nxrt::play
