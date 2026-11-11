// SPDX-License-Identifier: GPL-3.0-or-later
// The ini editor (nxrt/ini_edit.h).
#include "nxrt/ini_edit.h"

#include <cstdio>
#include <string>
#include <vector>

#include "ini_line.h"
#include "nxrt/file_write.h"
#include "nxrt/text.h"

namespace nxrt {
namespace {

// A value as the reader reads it back (ini_line.h): quoted when its ends are white space, when a comment mark follows white space in it or
// when it starts with a quote. A value holding a quote as well cannot be written quoted and goes as it is.
std::string format_value(std::string_view v) {
    std::string s;
    for (char c : v)
        if (c != '\r' && c != '\n') s += c;
    bool needs = !s.empty() && (text::is_space(s.front()) || text::is_space(s.back()) || s.front() == '"');
    for (size_t i = 1; i < s.size() && !needs; ++i) needs = (s[i] == ';' || s[i] == '#') && text::is_space(s[i - 1]);
    if (needs && s.find('"') == std::string::npos) return '"' + s + '"';
    return s;
}

}  // namespace

std::string ini_set_value(std::string_view text, std::string_view section, std::string_view key, std::string_view value) {
    const std::string sec = text::lower(std::string(text::trim(section)));
    const std::string name = text::lower(std::string(text::trim(key)));
    const std::string val = format_value(value);
    const bool crlf = text.empty() || text.find("\r\n") != std::string_view::npos;
    const bool final_eol = text.empty() || text.back() == '\n';

    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view l = text.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        lines.emplace_back(l);
        start = nl + 1;
    }

    // Keys before any header belong to section "" (as the reader sees them).
    std::string cur;
    bool found = false, in_section = sec.empty();
    size_t insert_at = 0;                  // after the section's last non-blank line (its header at least)
    for (size_t i = 0; i < lines.size(); ++i) {
        const ini_line::Line l = ini_line::parse(lines[i]);
        if (l.kind == ini_line::Kind::Section) {
            cur = l.name;
            if (cur == sec) {
                in_section = true;
                insert_at = i + 1;
            }
            continue;
        }
        if (cur != sec) continue;
        if (!text::trim(std::string_view(lines[i])).empty()) insert_at = i + 1;
        if (l.kind == ini_line::Kind::Key && l.name == name) {
            lines[i].replace(l.value_begin, l.value_end - l.value_begin, val);
            found = true;
        }
    }
    if (!found) {
        std::string line = std::string(text::trim(key)) + " = " + val;
        if (in_section) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(line));
        } else {
            if (!lines.empty() && !text::trim(std::string_view(lines.back())).empty()) lines.emplace_back();
            lines.push_back("[" + std::string(text::trim(section)) + "]");
            lines.push_back(std::move(line));
        }
    }

    const char* eol = crlf ? "\r\n" : "\n";
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += eol;
        out += lines[i];
    }
    if (final_eol) out += eol;
    return out;
}

bool ini_file_set_value(const std::string& path, std::string_view section, std::string_view key, std::string_view value, std::string* err) {
    std::string text;
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            if (err) *err = "cannot open " + path;
            return false;
        }
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    const std::string edited = ini_set_value(text, section, key, value);
    if (edited == text) return true;
    return write_file_atomic(path, edited, err);
}

}  // namespace nxrt
