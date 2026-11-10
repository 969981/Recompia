// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings file writer (format_settings): the sections in the table's order, each fixed key by its row
// (play_config_table.h); [padmap], [switch] and [bindings.*] are written here. The file format is described in play_types.h.
#include <string>

#include "nxrt/game.h"
#include "play_config_internal.h"

namespace nxrt::play {

using namespace cfg;

std::string format_settings(const Settings& s) {
    std::string o;
    auto kv = [&o](const char* key, const std::string& value) { o += std::string(key) + " = " + value + "\n"; };

    const char* title = game::info().title;
    o += (title && *title) ? std::string("# ") + title + ", play mode: controls and display.\n" : std::string("# Play mode: controls and display.\n");
    for (const SettingSection& sec : setting_sections()) {
        o += std::string("\n[") + sec.name + "]\n";
        switch (sec.kind) {
            case SectionKind::Rows:
                for (const SettingDef& d : settings_table()) {
                    if (setting_written(d) && std::string(d.section) == sec.name) kv(setting_key(d), setting_write(d, s));
                }
                break;
            case SectionKind::PadMap:
                for (int i = 0; i < kPadSrcCount; ++i) {
                    Output out;
                    out.buttons = s.pad.remap[i];
                    kv(pad_src_name(static_cast<PadSrc>(i)), s.pad.remap[i] == kPadDefault ? std::string("default") : output_text(out));
                }
                break;
            case SectionKind::Switch:
                for (int a = 1; a < kActCount; ++a) {   // every action except none
                    kv(act_info(static_cast<Act>(a)).name, output_text(s.out[a]));
                }
                break;
        }
    }

    // Always written, even when empty, so that a file with every binding removed does not fall back to the defaults.
    for (int c = 0; c < kCtxCount; ++c) {
        o += std::string("\n[bindings.") + ctx_name(static_cast<Ctx>(c)) + "]\n";
        for (const Binding& b : s.bindings) {
            if (static_cast<int>(b.ctx) != c) continue;
            const std::string key = input_name(b.in);
            if (key.empty()) continue;   // an input with no name cannot be written; it cannot be read back either
            o += key + " = " + act_info(b.act).name;
            if (b.trig != Trig::Press || b.ms != 0) o += std::string(" ") + trig_name(b.trig);
            if (b.ms != 0) o += " " + std::to_string(b.ms);
            o += "\n";
        }
    }
    return o;
}

}  // namespace nxrt::play
