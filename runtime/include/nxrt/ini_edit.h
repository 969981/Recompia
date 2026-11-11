// SPDX-License-Identifier: GPL-3.0-or-later
// Changing one value of an ini file the way a person would: the line of `key` in `[section]` gets the new value and every other line,
// comment, blank line, unknown key and the file's line endings stay as they are. Names are case-insensitive and lines are read with the
// config reader's own grammar (nxrt/config.h, src/diag/ini_line.h). The runtime's twin of the launcher's ini editor (launcher ini.rs).
#pragma once
#include <string>
#include <string_view>

namespace nxrt {

// `text` with [section] key = value. Every line of the key in the section gets the value in place (its spelling, the spacing around '='
// and a trailing comment stay); a missing key is added as "key = value" after the section's last line, a missing section at the end.
// The value is quoted when the reader would otherwise cut or trim it (a ';' or '#' after white space, white space at either end); line
// breaks in it are dropped. The line ending is the text's own (CRLF when it has one, else LF; an empty text gets CRLF).
std::string ini_set_value(std::string_view text, std::string_view section, std::string_view key, std::string_view value);

// The same on a file on disk, written through write_file_atomic (nxrt/file_write.h); the file is left alone when nothing changes. The file
// must exist: a missing file is an error (*err), never created, since where the settings file lives is the caller's to decide.
bool ini_file_set_value(const std::string& path, std::string_view section, std::string_view key, std::string_view value, std::string* err);

}  // namespace nxrt
