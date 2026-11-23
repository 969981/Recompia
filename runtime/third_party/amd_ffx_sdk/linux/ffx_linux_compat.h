// SPDX-License-Identifier: MIT
// Recompia: force-included into every FidelityFX SDK source built on Linux (runtime/cmake/ffx_sdk.cmake). The SDK v1.1.x
// sources assume MSVC; these are the few Windows spellings they use, mapped to their standard equivalents.
#pragma once
#ifdef __cplusplus
#include <cmath>
#include <cwchar>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <codecvt>
#include <locale>
#include <string>
template <size_t N> inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) { wcsncpy(dst, src, N - 1); dst[N - 1] = 0; return 0; }
inline int wcscpy_s(wchar_t* dst, size_t n, const wchar_t* src) { wcsncpy(dst, src, n - 1); dst[n - 1] = 0; return 0; }
template <size_t N> inline int strcpy_s(char (&dst)[N], const char* src) { strncpy(dst, src, N - 1); dst[N - 1] = 0; return 0; }
inline int strcpy_s(char* dst, size_t n, const char* src) { strncpy(dst, src, n - 1); dst[n - 1] = 0; return 0; }
constexpr unsigned long long operator""ui64(unsigned long long v) { return v; }   // MSVC's 64-bit literal suffix
#else
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#define __declspec(x)
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define swprintf_s swprintf
#define sprintf_s snprintf
#define FFX_UNUSED(x)               ((void)(x))
#define _wcsicmp wcscasecmp
