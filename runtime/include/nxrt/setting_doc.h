// SPDX-License-Identifier: GPL-3.0-or-later
// The description of an ini setting, without its binding to any struct: what the docs (runtime/docs/CONFIG.md) and the launcher's schemas
// show. Game-independent and file-independent: the play-mode table (src/gfx/play/play_config_table.h) fills these rows for controls.ini,
// a game module fills them for the keys of its own section of the game ini (game::Info::settings_doc), and both use the same two emitters
// (src/gfx/play/setting_doc.cpp). The struct and number_text() are header-only, so a pure-data game library can fill rows.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::setting_doc {

struct Row {
    std::string section;                 // "display"
    std::string key;                     // "fps_limit"
    std::string type;                    // "bool" "int" "real" "text" "choice", or a value syntax of its own ("key", "pad_combo", "ratio")
    std::string def;                     // the default, spelled as the file writes it; empty when the key is not written
    bool optional = false;               // no default: while the key is unset something else decides (the doc says what); def is empty
    bool has_min = false, has_max = false;
    double min = 0, max = 0;             // the accepted range of a number
    bool clamped = false;                // a number outside min..max is clamped into it (else it is refused and the default kept)
    bool zero_ok = false;                // 0 is accepted as well (outside min..max)
    std::vector<std::string> choices;    // the accepted words (the first spelling of each), in order
    // per choice word, what the settings pages show with it (the prompts style: "A=xbox/a", the icon of each Switch button); schema only
    std::vector<std::pair<std::string, std::vector<std::string>>> choice_details;
    std::string auto_word;               // a word accepted instead of a number ("auto")
    std::vector<std::string> aliases;    // older key names that are read too
    std::string fallback;                // a key of the same section whose value this one takes when the file has that one and not this one
    std::string nxrt_key;                // "nvn.pool_mirror": the nxrt.ini key (or its NXRT_ variable) that wins over this one when set
    std::string doc;                     // one line: what it does
    std::string group;                   // a set of keys the settings pages show together ("gameplay": a gameplay mod); empty = none
    std::string parent;                  // a key of the same group this one belongs to: the settings pages show it under that key
    bool opt_in = false;                 // a group member that is off by default on purpose (a "gameplay" mod the player switches on)
    bool hidden = false;                 // a group member the settings pages show no row for (the ini still takes it)
    bool read_only = false;             // read, never written (an alternative spelling of other keys)
    bool retired = false;                // read and ignored without a warning; never written
};

// Where the rows live, which decides what overrides a key:
//   OwnFile  a file of its own (controls.ini): the game-ini key in Row::nxrt_key, or its NXRT_ variable, wins when set
//   GameIni  the game ini itself (nxrt.ini / pkp.ini, nxrt/config.h): each key's own NXRT_ variable wins
enum class Store : uint8_t { OwnFile, GameIni };

// A number as the docs and the schemas spell it: the shortest %g that reads back.
inline std::string number_text(double v) {
    char buf[64];
    for (int digits = 6; digits <= 17; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*g", digits, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

// The environment variable of an nxrt.ini key: NXRT_ + SECTION + _ + KEY, every other character than a letter or digit as '_'.
std::string env_name(const std::string& section, const std::string& key);

// A Markdown table of the live rows (Section, Key, Default, Values, Overridden by or Environment variable, Meaning) and a list of the
// retired ones.
std::string markdown(const std::vector<Row>& rows, Store store = Store::OwnFile);
// A JSON document: {"file": file, "sections": [{"name", "keys": [...]}], "retired": [...]}, sections in the order of their first row.
// A key with choice_details carries "choice_details": {word: [...], ...}.
// A GameIni key carries "env" (its own variable); an OwnFile key "overridden_by" when it has an nxrt_key.
std::string schema_json(const std::string& file, const std::vector<Row>& rows, Store store = Store::OwnFile);

}  // namespace nxrt::setting_doc
