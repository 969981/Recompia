// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the game's factory settings and their migration into a settings file written by an older build.
#include <string>

#include "nxrt/game.h"
#include "play_config_internal.h"

namespace nxrt::play {
namespace cfg {

// The factory-defaults revision this build writes ([meta] defaults_rev) and the factory bindings an older controls.ini does not have: the
// game's (play_types.h GameDefaults).
int defaults_rev() {
    const GameDefaults* d = game_defaults();
    return d ? d->defaults_rev : 0;
}

// Factory values a later revision changed (GameDefaults::changes): where the file still has the old factory value, it takes the new one.
// "Still has" = setting the old value changes nothing in the written file, so any value the player picked stays.
void apply_default_changes(Settings* s, int file_rev, std::string* added) {
    const GameDefaults* gd = game_defaults();
    for (unsigned i = 0; gd && i < gd->change_count; ++i) {
        const GameDefaultChange& c = gd->changes[i];
        const Sec sec = fixed_sec(lower(c.section));
        if (c.rev <= file_rev || sec == Sec::Unknown) continue;
        Settings old = *s, now = *s;
        bool known = false;
        std::string msg;
        if (!apply_fixed(sec, lower(c.key), c.from, &old, &known, &msg) || !known) continue;
        if (format_settings(old) != format_settings(*s)) continue;
        if (!apply_fixed(sec, lower(c.key), c.to, &now, &known, &msg) || !known) continue;
        *s = now;
        if (!added->empty()) *added += ", ";
        *added += std::string(c.key) + " = " + c.to + " in [" + c.section + "]";
    }
}

void apply_default_additions(Settings* s, int file_rev, std::string* added) {
    const GameDefaults* gd = game_defaults();
    for (unsigned i = 0; gd && i < gd->addition_count; ++i) {
        const GameDefaultAddition& d = gd->additions[i];
        if (d.rev <= file_rev) continue;
        InputId id;
        if (!parse_input(d.input, &id)) continue;
        bool taken = false;
        for (const Binding& b : s->bindings) {
            if (b.ctx == d.ctx && b.in == id) {
                taken = true;
                break;
            }
        }
        if (taken) continue;
        Binding b;
        b.in = id;
        b.ctx = d.ctx;
        b.act = d.act;
        b.trig = Trig::Press;
        b.ms = 0;
        s->bindings.push_back(b);
        if (!added->empty()) *added += ", ";
        *added += input_name(id) + " = " + act_info(d.act).name + " in [bindings." + ctx_name(d.ctx) + "]";
    }
}

}  // namespace cfg

using namespace cfg;

const GameDefaults* game_defaults() { return game::info().play_defaults; }

const char* default_settings_text() {
    const GameDefaults* d = game_defaults();
    return d && d->settings_text ? d->settings_text : "";
}

Settings default_settings() {
    Settings s;
    std::string err;
    // The factory text has a [bindings.*] section, so parse_settings does not look at the factory bindings again.
    if (!parse_settings(default_settings_text(), &s, &err)) {
        s = builtin_settings();
        s.defaults_rev = defaults_rev();
    }
    return s;
}

}  // namespace nxrt::play
