// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings table of controls.ini. One row per key of the fixed sections: its section, key, value type, range or words,
// the Settings member it binds, its older names, the nxrt.ini key that wins over it, and one line of docs. The reader
// (play_config_apply.cpp), the writer (play_config_format.cpp), runtime/docs/CONFIG.md and the launcher's schema
// (runtime/docs/controls_schema.json, runtime/scripts/gen_settings_doc.ps1) all come from it, so a new key is one row here plus its
// widget on the settings page. The defaults are the Settings struct's (play_types.h) under the game's factory text.
// [padmap], [switch] and [bindings.*] have rows of their own kind (one per pad button, action or input) and keep their own code.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "nxrt/setting_doc.h"
#include "play_types.h"

namespace nxrt::play {

enum class SettingType : uint8_t {
    Bool,      // 1/0 (true/false, yes/no, on/off read)
    Int,       // whole number, lo..hi (zero_ok: or 0; allowed: one of a list); auto_word: written for a value <= auto_at, read as auto_at
    Real,      // number, lo..hi when ranged (zero_ok: or 0); exact: written with as many digits as it takes to read back the same
    Text,      // kept as written; auto_word: written for "" and read as ""; empty_as: what an empty value reads as
    Choice,    // one of `words` (case-insensitive); bound to an enum (get/set, the word's index) or to a string (text, the word)
    Custom,    // read / write of its own
    Retired,   // a key older builds wrote: read and ignored without a warning, never written
};

struct SettingDef {
    const char* section;   // lower case, no brackets
    const char* key;       // lower case; nullptr: key_fn names it
    SettingType type;
    const char* (*key_fn)() = nullptr;                // a key the game may rename (GameVocabulary)
    // the binding, by type
    bool& (*flag)(Settings&) = nullptr;               // Bool
    int& (*whole)(Settings&) = nullptr;               // Int
    double& (*real)(Settings&) = nullptr;             // Real
    std::string& (*text)(Settings&) = nullptr;        // Text, Choice bound to a string
    int (*get)(const Settings&) = nullptr;            // Choice bound to an enum
    void (*set)(Settings&, int) = nullptr;
    bool (*read)(const std::string& k, const std::string& v, Settings* s, std::string* msg) = nullptr;   // Custom
    std::string (*write)(const Settings& s) = nullptr;                                                     // Custom; nullptr: read only
    // what the value may be
    std::span<const char* const> words = {};          // Choice: the file's words, in enum order
    std::vector<std::string> (*word_fn)() = nullptr;  // Custom: the words, when another table owns them (docs and launcher only)
    // Custom choice: per word, what the settings pages show with it, from the factory settings (setting_doc::Row::choice_details)
    std::vector<std::pair<std::string, std::vector<std::string>>> (*details_fn)(const Settings& defaults) = nullptr;
    std::span<const int> allowed = {};                // Int: the only values accepted
    double lo = 0, hi = 0;
    bool ranged = false;                              // Real: lo..hi applies (Int always has one, unless `allowed`)
    bool zero_ok = false;                             // 0 is accepted outside lo..hi
    bool exact = false;                               // Real: written by num_exact (else num)
    const char* auto_word = nullptr;
    int auto_at = 0;
    const char* empty_as = nullptr;
    // Custom: the value's syntax for the docs and the launcher ("choice", "key", "pad_combo", "ratio", "real"); its words go in `words`,
    // its range in lo / hi / ranged (only described there; read checks them itself)
    const char* syntax = nullptr;
    const char* renamed_from = nullptr;               // an older name of the key, read too
    const char* fallback = nullptr;                   // a key of the section whose value this one takes when the file has that one and not this one
    const char* nxrt_key = nullptr;                   // "section.key" of nxrt.ini / the game ini (or its NXRT_ variable) that wins over this one
    const char* doc = nullptr;                        // one line for the docs and the launcher
};

// The sections of the file in the order the writer writes them. Rows: the table's rows. PadMap, Switch: their own code.
enum class SectionKind : uint8_t { Rows, PadMap, Switch };
struct SettingSection {
    const char* name;
    SectionKind kind;
};
std::span<const SettingSection> setting_sections();
const SettingSection* find_section(const std::string& lower_name);   // nullptr: no such section (retired ones included)
bool retired_section(const std::string& lower_name);                 // [aim] [camera]: older sections, skipped without a warning

std::span<const SettingDef> settings_table();
// The row of `key` (lower case) in `section`, its older name included; -1 for none.
int find_setting(const std::string& section, const std::string& key);
const char* setting_key(const SettingDef& d);
bool setting_written(const SettingDef& d);   // false for a retired key and a read-only one (an alternative spelling of others)
// Applies one value. On a bad value returns false with *msg set and leaves *s as it was. k is the key as the file spelled it (lower case).
bool setting_read(const SettingDef& d, const std::string& k, const std::string& v, Settings* s, std::string* msg);
// The value as the file writes it.
std::string setting_write(const SettingDef& d, const Settings& s);
// to's value becomes from's (both rows of one type: Bool, Int, Real or Text).
void setting_copy(const SettingDef& from, const SettingDef& to, Settings* s);

// Actions, contexts of older files that no longer exist (compared without case, '-' as '_'). A [switch] row, a binding or a
// [bindings.<ctx>] section that uses one is skipped without a warning.
bool retired_act(const std::string& name);
// The retired actions that pressed a button by default (confirm = A, back = B): the action a binding of an older file gets in their place.
const char* replaced_act(const std::string& name);
bool retired_ctx(const std::string& name);

// The table as docs rows, with the defaults of default_settings() (the registered game's factory settings).
std::vector<setting_doc::Row> settings_doc_rows();

}  // namespace nxrt::play
