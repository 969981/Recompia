// SPDX-License-Identifier: GPL-3.0-or-later
// Small text helpers for the whole runtime (ini and environment values, names, log dumps). Header-only, no locale: lower() touches ASCII letters
// only and trim() strips the C-locale white space (space, tab, newline, vertical tab, form feed, carriage return).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nxrt::text {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// ASCII lower case.
inline std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return s;
}

// ASCII upper case.
inline std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    return s;
}

// s without leading and trailing white space.
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}
inline std::string trim(const std::string& s) { return std::string(trim(std::string_view(s))); }

inline std::string lower_trim(const std::string& s) { return lower(trim(s)); }

// Strips leading and trailing spaces and tabs only, in place.
inline void trim_blanks(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '	')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '	')) s.erase(s.begin());
}

// The first max_bytes bytes of b as hex ("|" every 16 bytes, "..." when cut). For the Debug lines that dump a payload.
inline std::string hex_head(const std::vector<uint8_t>& b, size_t max_bytes) {
    static const char kHex[] = "0123456789abcdef";
    const size_t n = b.size() < max_bytes ? b.size() : max_bytes;
    std::string s;
    s.reserve(n * 3 + n / 16 * 2 + 4);
    for (size_t i = 0; i < n; ++i) {
        if (i) s += (i % 16) ? " " : " | ";
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 15];
    }
    if (n < b.size()) s += " ...";
    return s;
}

// The largest n' <= n that does not cut inside a UTF-8 sequence of s (n itself when s[n] starts a character or n >= s.size()).
inline size_t utf8_cut(const std::string& s, size_t n) {
    while (n && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

}  // namespace nxrt::text
