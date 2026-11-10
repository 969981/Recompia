// SPDX-License-Identifier: GPL-3.0-or-later
// Text helpers of the settings file reader and writer (play_config*.cpp): trimming, number and word conversion, line splitting.
#pragma once
#include <span>
#include <string>
#include <vector>

#include "nxrt/text.h"

namespace nxrt::play::cfg {

inline const std::string::size_type kNpos = std::string::npos;

using text::lower;
using text::trim;
std::string num(double v);                 // "%.9g" for doubles, as the file format asks
std::string num_exact(double v);           // as num(), but a value that would not read back to the same double gets all 17 digits
bool to_int(const std::string& v, long long* out);
bool to_double(const std::string& v, double* out);
bool to_bool(const std::string& v, bool* out);
bool pick_word(const std::string& k, const std::string& v, std::span<const char* const> words, int* idx, std::string* msg);
std::vector<std::string> split_lines(const std::string& text);   // splits on '\n'; line i of the result is line i+1 of the file
std::string strip_inline_comment(const std::string& line);       // cuts a trailing comment: a '#' or ';' that follows a space or tab

}  // namespace nxrt::play::cfg
