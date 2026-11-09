// SPDX-License-Identifier: GPL-3.0-or-later
// Small text helpers shared by services/ and net/. Header-only. A runtime-wide header (include/nxrt/) could replace this file when the
// other directories' copies of these helpers are unified.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxrt::textutil {

// ASCII-only lower case (no locale).
inline std::string ascii_lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return s;
}

// Strips leading and trailing spaces and tabs.
inline void trim_blanks(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
}

// The largest n' <= n that does not cut inside a UTF-8 sequence of s (n itself when s[n] starts a character or n >= s.size()).
inline size_t utf8_cut(const std::string& s, size_t n) {
    while (n && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

}  // namespace nxrt::textutil
