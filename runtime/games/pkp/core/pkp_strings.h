// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: our own player-visible strings in the game's language (the language at the call, pkp_language.h). The keys and their
// English text are in pkp_strings_table.h, the other languages in pkp_strings_<lang>.inc; a language without a key does not compile.
//   strings::get(base, Str::Key)             the text
//   strings::fmt(base, Str::Key, a, b, ...)  the text with {0}, {1}, ... replaced by the arguments (strings or numbers), by position
// Logs (NXLOG) stay English and do not use this.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace nxrt::games::pkp::strings {

enum class Str : uint16_t {
#define S(key, draw, en) key,
#include "core/pkp_strings_table.h"
#undef S
};

const char* get(uint64_t base, Str key);
std::string fmt_args(uint64_t base, Str key, const std::string* args, size_t count);

namespace detail {
inline std::string arg(std::string_view s) { return std::string(s); }
inline std::string arg(const char* s) { return s ? std::string(s) : std::string(); }
template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
std::string arg(T v) {
    return std::to_string(v);
}
}  // namespace detail

template <class... A>
std::string fmt(uint64_t base, Str key, const A&... args) {
    const std::string parts[] = {detail::arg(args)..., std::string()};   // the last one keeps the array non-empty
    return fmt_args(base, key, parts, sizeof...(A));
}

}  // namespace nxrt::games::pkp::strings
