// SPDX-License-Identifier: GPL-3.0-or-later
// Text helpers of the settings file reader and writer, see ini_text.h.
#include "ini_text.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace nxrt::play::cfg {

// "%.9g" for doubles, as the file format asks.
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// As num(), but a value that would not read back to the same double gets all 17 digits (16/9 needs them).
std::string num_exact(double v) {
    std::string s = num(v);
    if (std::strtod(s.c_str(), nullptr) != v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return buf;
    }
    return s;
}

// Whole numbers and reals: the whole string must be a number, and reals must be finite.
bool to_int(const std::string& v, long long* out) {
    if (v.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (errno != 0 || end != v.c_str() + v.size()) return false;
    *out = x;
    return true;
}

bool to_double(const std::string& v, double* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (end != v.c_str() + v.size() || !std::isfinite(x)) return false;
    *out = x;
    return true;
}

bool to_bool(const std::string& v, bool* out) {
    const std::string l = lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") {
        *out = true;
        return true;
    }
    if (l == "0" || l == "false" || l == "no" || l == "off") {
        *out = false;
        return true;
    }
    return false;
}

// One word of a fixed list (case-insensitive): its index in `words`. A word that is none of them sets *msg listing the allowed ones.
bool pick_word(const std::string& k, const std::string& v, std::span<const char* const> words, int* idx, std::string* msg) {
    const std::string l = lower(v);
    for (size_t i = 0; i < words.size(); ++i) {
        if (l == words[i]) {
            *idx = static_cast<int>(i);
            return true;
        }
    }
    std::string allowed;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) allowed += i + 1 == words.size() ? " or " : ", ";
        allowed += words[i];
    }
    *msg = k + " must be " + allowed + ", got '" + v + "'";
    return false;
}

// Splits on '\n'. Line i of the result is line i+1 of the file (for error messages).
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        if (nl == kNpos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

// Cuts a trailing comment: a '#' or ';' that follows a space or tab.
std::string strip_inline_comment(const std::string& line) {
    for (size_t i = 1; i < line.size(); ++i) {
        if ((line[i] == '#' || line[i] == ';') && (line[i - 1] == ' ' || line[i - 1] == '\t')) return trim(line.substr(0, i));
    }
    return line;
}

}  // namespace nxrt::play::cfg
