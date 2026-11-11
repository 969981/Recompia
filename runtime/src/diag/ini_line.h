// SPDX-License-Identifier: GPL-3.0-or-later
// One line of an ini file as the config reader takes it (nxrt/config.h): the grammar shared by the reader (config.cpp) and the editor
// that changes one value in place (ini_edit.cpp), so the two never disagree on what a line means. Internal to src/diag.
//   [section]                 a section header (name trimmed, lower case; text after the ']' is ignored)
//   key = value               a key (trimmed, lower case); the value is trimmed, and a ';' or '#' after white space starts a comment
//   key = "value"             quoted: the text between the quotes, verbatim; anything after the closing quote is ignored
//   ; comment   # comment     whole-line comments; blank lines and lines without '=' are skipped
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "nxrt/text.h"

namespace nxrt::ini_line {

enum class Kind : uint8_t { Other, Section, Key };   // Other: blank, comment or a line the reader skips

struct Line {
    Kind kind = Kind::Other;
    std::string name;                 // Section: the section; Key: the key (both trimmed, lower case)
    std::string value;                // Key: the value as the reader takes it
    size_t value_begin = 0;           // Key: the value's text in the line, quotes included (an empty value: both at the end of the
    size_t value_end = 0;             //      white space after '='), so an edit can replace it and keep a trailing comment
};

inline Line parse(std::string_view line) {
    using text::is_space;
    Line out;
    size_t lead = 0;
    while (lead < line.size() && is_space(line[lead])) ++lead;
    size_t end = line.size();
    while (end > lead && is_space(line[end - 1])) --end;
    const std::string_view s = line.substr(lead, end - lead);
    if (s.empty() || s[0] == ';' || s[0] == '#') return out;
    if (s[0] == '[') {
        const size_t close = s.find(']');
        if (close == std::string_view::npos) return out;
        out.kind = Kind::Section;
        out.name = text::lower(std::string(text::trim(s.substr(1, close - 1))));
        return out;
    }
    const size_t eq = s.find('=');
    if (eq == std::string_view::npos) return out;
    out.name = text::lower(std::string(text::trim(s.substr(0, eq))));
    if (out.name.empty()) return out;
    out.kind = Kind::Key;
    const size_t at = lead + eq + 1;   // the rest of the line after '=', in line positions
    const std::string_view rest = s.substr(eq + 1);
    const size_t first = rest.find_first_not_of(" \t");
    if (first != std::string_view::npos && rest[first] == '"') {
        const size_t close = rest.find('"', first + 1);
        if (close != std::string_view::npos) {
            out.value = std::string(rest.substr(first + 1, close - first - 1));
            out.value_begin = at + first;
            out.value_end = at + close + 1;
        } else {
            out.value = std::string(text::trim(rest.substr(first + 1)));
            out.value_begin = at + first;
            out.value_end = at + rest.size();
        }
        return out;
    }
    size_t cut = rest.size();
    for (size_t i = 1; i < rest.size(); ++i) {
        if ((rest[i] == ';' || rest[i] == '#') && is_space(rest[i - 1])) {
            cut = i;
            break;
        }
    }
    size_t b = 0, e = cut;
    while (b < e && is_space(rest[b])) ++b;
    while (e > b && is_space(rest[e - 1])) --e;
    out.value = std::string(rest.substr(b, e - b));
    out.value_begin = at + b;
    out.value_end = at + e;
    return out;
}

}  // namespace nxrt::ini_line
