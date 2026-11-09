// SPDX-License-Identifier: GPL-3.0-or-later
// The description of an ini setting, without its binding to any struct: what the docs (runtime/docs/CONFIG.md) and the launcher's schema
// show. Game-independent and file-independent: the play-mode table (play_config_table.h) fills these rows for controls.ini, and any
// other table of ini keys (a game layer's) can fill the same rows and use the same two emitters.
#pragma once
#include <string>
#include <vector>

namespace nxrt::setting_doc {

struct Row {
    std::string section;                 // "display"
    std::string key;                     // "fps_limit"
    std::string type;                    // "bool" "int" "real" "text" "choice", or a value syntax of its own ("key", "pad_combo", "ratio")
    std::string def;                     // the default, spelled as the file writes it; empty when the key is not written
    bool has_min = false, has_max = false;
    double min = 0, max = 0;             // the accepted range of a number
    bool zero_ok = false;                // 0 is accepted as well (outside min..max)
    std::vector<std::string> choices;    // the accepted words (the first spelling of each), in order
    std::string auto_word;               // a word accepted instead of a number ("auto")
    std::vector<std::string> aliases;    // older key names that are read too
    std::string fallback;                // a key of the same section whose value this one takes when the file has that one and not this one
    std::string nxrt_key;                // "nvn.pool_mirror": the nxrt.ini key (or its NXRT_ variable) that wins over this one when set
    std::string doc;                     // one line: what it does
    bool read_only = false;              // read, never written (an alternative spelling of other keys)
    bool retired = false;                // read and ignored without a warning; never written
};

// The environment variable of an nxrt.ini key: NXRT_ + SECTION + _ + KEY, every other character than a letter or digit as '_'.
std::string env_name(const std::string& section, const std::string& key);

// A Markdown table of the live rows (Section, Key, Default, Values, Overridden by, Meaning) and a list of the retired ones.
std::string markdown(const std::vector<Row>& rows);
// A JSON document: {"file": file, "sections": [{"name", "keys": [...]}], "retired": [...]}, sections in the order of their first row.
std::string schema_json(const std::string& file, const std::vector<Row>& rows);

}  // namespace nxrt::setting_doc
