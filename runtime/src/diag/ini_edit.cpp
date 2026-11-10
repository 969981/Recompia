// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/ini_edit.h"

#include "nxrt/text.h"

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>

namespace nxrt {
namespace {

using text::lower;
using text::trim;

struct Line {
    size_t begin, end;   // the line's text in the file, without its line ending
    size_t next;         // where the next line starts
};

std::vector<Line> split_lines(const std::string& t) {
    std::vector<Line> out;
    size_t at = 0;
    while (at < t.size()) {
        size_t nl = t.find('\n', at);
        size_t stop = nl == std::string::npos ? t.size() : nl;
        size_t end = stop > at && t[stop - 1] == '\r' ? stop - 1 : stop;
        out.push_back({at, end, nl == std::string::npos ? t.size() : nl + 1});
        at = out.back().next;
    }
    return out;
}

// "[name]" -> the lower-cased name; false when the line is not a section header.
bool section_of(std::string_view line, std::string* name) {
    std::string s = trim(std::string(line));
    if (s.empty() || s[0] != '[') return false;
    size_t close = s.find(']');
    if (close == std::string::npos) return false;
    *name = lower(trim(s.substr(1, close - 1)));
    return true;
}

// The value of a "key = value" line: where the value starts and ends inside the line (a quoted value includes its quotes).
struct Span {
    std::string key;
    size_t value_begin, value_end;
};

bool key_of(const std::string& line, Span* out) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == ';' || line[i] == '#' || line[i] == '[') return false;
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    std::string key = lower(trim(line.substr(i, eq - i)));
    if (key.empty()) return false;
    size_t vb = line.find_first_not_of(" \t", eq + 1);
    if (vb == std::string::npos) vb = line.size();
    size_t ve;
    if (vb < line.size() && line[vb] == '"') {
        size_t close = line.find('"', vb + 1);
        ve = close == std::string::npos ? line.size() : close + 1;
    } else {
        ve = line.size();
        for (size_t k = vb + 1; k < line.size(); ++k)
            if ((line[k] == ';' || line[k] == '#') && text::is_space(line[k - 1])) {
                ve = k;
                break;
            }
        while (ve > vb && text::is_space(line[ve - 1])) --ve;   // the white space before a comment stays with the comment
    }
    *out = {key, vb, ve};
    return true;
}

std::string written(const std::string& v) {
    bool quote = v.empty() || text::is_space(v.front()) || text::is_space(v.back()) || v.find_first_of("\";#") != std::string::npos;
    return quote ? '"' + v + '"' : v;
}

}  // namespace

std::string ini_with_value(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    const std::string want_section = lower(trim(section)), want_key = lower(trim(key));
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::vector<Line> lines = split_lines(text);

    std::string current;
    int key_line = -1;        // the last line that sets the key inside the section
    Span key_span{};
    int last_in_section = -1;   // the section's last block: its header, or its last "key = value" line
    for (size_t n = 0; n < lines.size(); ++n) {
        const std::string line = text.substr(lines[n].begin, lines[n].end - lines[n].begin);
        std::string name;
        if (section_of(line, &name)) {
            current = name;
            if (current == want_section) last_in_section = static_cast<int>(n);   // an empty block still gets its key after the header
            continue;
        }
        if (current != want_section) continue;
        Span sp;
        if (!key_of(line, &sp)) continue;   // a comment or blank line before the next header may belong to that section: do not insert after it
        last_in_section = static_cast<int>(n);
        if (sp.key == want_key) {
            key_line = static_cast<int>(n);
            key_span = sp;
        }
    }

    if (key_line >= 0) {   // replace the value in place
        const Line& l = lines[key_line];
        std::string out = text;
        out.replace(l.begin + key_span.value_begin, key_span.value_end - key_span.value_begin, written(value));
        return out;
    }
    const std::string add = trim(key) + " = " + written(value);
    if (last_in_section >= 0) {   // add the key after the section's last line
        const Line& l = lines[last_in_section];
        std::string out = text.substr(0, l.end);
        out += eol + add;
        out += text.substr(l.end);   // the line's own ending and everything after it
        if (l.end == text.size()) out += eol;
        return out;
    }
    std::string out = text;   // add the section at the end
    if (!out.empty() && out.back() != '\n') out += eol;
    if (!out.empty() && !lines.empty() && !trim(text.substr(lines.back().begin, lines.back().end - lines.back().begin)).empty()) out += eol;
    return out + '[' + trim(section) + ']' + eol + add + eol;
}

bool ini_save_key(const char* path, const char* section, const char* key, const char* value, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (!path || !*path || !section || !key) return fail("no path, section or key");
    std::string old;
    if (std::FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) old.append(buf, n);
        const bool bad = std::ferror(f) != 0;
        std::fclose(f);
        if (bad) return fail(std::string("cannot read ") + path);
    }
    const std::string text = ini_with_value(old, section, key, value ? value : "");
    const std::string tmp = std::string(path) + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return fail("cannot write " + tmp);
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
        std::remove(tmp.c_str());
        return fail("cannot write " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);   // replaces the file, on Windows too
    if (ec) {
        std::remove(tmp.c_str());
        return fail("cannot replace " + std::string(path) + ": " + ec.message());
    }
    return true;
}

}  // namespace nxrt
