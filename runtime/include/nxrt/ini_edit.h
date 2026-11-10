// SPDX-License-Identifier: GPL-3.0-or-later
// Changing one value of an ini file without disturbing the rest of it: comments, key order, blank lines, other sections and the file's
// line endings stay as they were. The parsing rules are config.h's: sections and keys are case-insensitive, a quoted value is taken
// verbatim, an unquoted one ends at a ';' or '#' that follows white space, the last line that sets a key wins.
#pragma once
#include <string>

namespace nxrt {

// `text` with [section] key = value set. An existing key's value is replaced in place (its last line, with any comment after it kept);
// a missing key is added after the last line of the section; a missing section is added at the end. A value that would not read back
// as itself unquoted (empty, edge white space, a quote, ';' or '#') is written in double quotes. Pure: no file, no global state.
std::string ini_with_value(const std::string& text, const std::string& section, const std::string& key, const std::string& value);

// Applies ini_with_value to the file at `path` (a missing file counts as empty and is created) and replaces it atomically: the new
// text goes to "<path>.tmp" and is renamed over the file, so a reader never sees half a file. False with the reason in *err (when given)
// if the file cannot be read or written; the old file is then untouched.
bool ini_save_key(const char* path, const char* section, const char* key, const char* value, std::string* err = nullptr);

}  // namespace nxrt
