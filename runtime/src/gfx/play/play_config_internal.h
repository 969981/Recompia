// SPDX-License-Identifier: GPL-3.0-or-later
// Settings file code shared by play_config*.cpp: the reader (play_config.cpp, play_config_apply.cpp), the factory defaults and their
// migration (play_config_migrate.cpp) and the writer (play_config_format.cpp), all driven by the settings table (play_config_table.h).
// Not for use outside those files.
#pragma once
#include <string>
#include <vector>

#include "ini_text.h"
#include "play_config_table.h"
#include "play_types.h"

namespace nxrt::play::cfg {

// The state of the reader between section headers: before any, in a section it does not know or that is retired (its keys are skipped),
// in a [bindings.<ctx>] section, or in a fixed section (play_config_table.h SettingSection).
enum class Sec { None, Unknown, Retired, Bindings, Fixed };

// play_config.cpp
Settings builtin_settings();
bool parse_onto(const std::string& text, Settings* s, std::string* err, std::vector<std::string>* warnings, bool with_default_bindings);

// play_config_apply.cpp
// Applies one `key = value` (key in lower case) of a fixed section. On a bad value returns false with *msg set (no line prefix) and leaves *s
// as it was. *known is false for a key the section does not have. *row is the table row it set (-1: none, or a [padmap] / [switch] key).
bool apply_fixed(const SettingSection& sec, const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg, int* row);

// play_config_migrate.cpp
int defaults_rev();
void apply_default_changes(Settings* s, int file_rev, std::string* added);
void apply_default_additions(Settings* s, int file_rev, std::string* added);

}  // namespace nxrt::play::cfg
