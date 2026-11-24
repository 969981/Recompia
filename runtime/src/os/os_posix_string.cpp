// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_string.cpp: strings for src/os/nx_os.h -- UTF-8 <-> wchar_t conversion, the lstr* and ordinal compares,
// FormatMessageW, OutputDebugString and nxos_utf8.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace nxos;

// ==================================================================================================================
// string conversion (UTF-8 <-> UTF-32: wchar_t is 4 bytes here, 2 on Windows)
// ==================================================================================================================
namespace {

uint32_t utf8_next(const unsigned char*& p, const unsigned char* end) {
    const unsigned char b = *p;
    if (b < 0x80) {
        ++p;
        return b;
    }
    int extra;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0) {
        extra = 1;
        cp = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        extra = 2;
        cp = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        extra = 3;
        cp = b & 0x07;
    } else {
        ++p;
        return 0xFFFD;
    }
    if (p + extra >= end) {
        ++p;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        const unsigned char c = p[i];
        if ((c & 0xC0) != 0x80) {
            ++p;
            return 0xFFFD;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    p += extra + 1;
    return cp;
}

size_t utf8_put(uint32_t cp, char* out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

int wide_len(const wchar_t* s, int len) {
    if (len >= 0) return len;
    int n = 0;
    while (s[n]) ++n;
    return n + 1;   // a length of -1 includes the terminator, like Win32
}

int narrow_len(const char* s, int len) {
    if (len >= 0) return len;
    int n = 0;
    while (s[n]) ++n;
    return n + 1;
}

int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

}  // namespace

int MultiByteToWideChar(UINT cp, DWORD, LPCSTR src, int src_len, LPWSTR dst, int dst_len) {
    if (!src) return 0;
    if (cp != CP_UTF8 && cp != CP_ACP && cp != CP_OEMCP) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int n = narrow_len(src, src_len);
    const auto* p = reinterpret_cast<const unsigned char*>(src);
    const auto* end = p + n;
    if (!dst || dst_len == 0) {
        int count = 0;
        while (p < end) {
            if (*p == 0 && src_len < 0) {
                ++count;
                break;
            }
            utf8_next(p, end);
            ++count;
        }
        return count;
    }
    int written = 0;
    while (p < end && written < dst_len) {
        if (*p == 0 && src_len < 0) {
            dst[written++] = 0;
            break;
        }
        dst[written++] = static_cast<wchar_t>(utf8_next(p, end));
    }
    return written;
}

int WideCharToMultiByte(UINT cp, DWORD, LPCWSTR src, int src_len, LPSTR dst, int dst_len, LPCSTR, LPBOOL) {
    if (!src) return 0;
    if (cp != CP_UTF8 && cp != CP_ACP && cp != CP_OEMCP) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int n = wide_len(src, src_len);
    if (!dst || dst_len == 0) {
        int count = 0;
        char tmp[4];
        for (int i = 0; i < n; i++) {
            const wchar_t c = src[i];
            if (c == 0 && src_len < 0) {
                ++count;
                break;
            }
            count += static_cast<int>(utf8_put(static_cast<uint32_t>(c), tmp));
        }
        return count;
    }
    int written = 0;
    for (int i = 0; i < n && written < dst_len; i++) {
        const wchar_t c = src[i];
        if (c == 0 && src_len < 0) {
            dst[written++] = 0;
            break;
        }
        char tmp[4];
        const size_t k = utf8_put(static_cast<uint32_t>(c), tmp);
        if (written + static_cast<int>(k) > dst_len) break;
        for (size_t j = 0; j < k; j++) dst[written++] = tmp[j];
    }
    return written;
}

int lstrlenW(LPCWSTR s) {
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

int lstrlenA(LPCSTR s) {
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

LPWSTR lstrcpyW(LPWSTR dst, LPCWSTR src) {
    LPWSTR d = dst;
    while (src && (*d++ = *src++)) {
    }
    return dst;
}

int lstrcmpiW(LPCWSTR a, LPCWSTR b) {
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    for (;; ++a, ++b) {
        const int x = ascii_lower(static_cast<int>(*a));
        const int y = ascii_lower(static_cast<int>(*b));
        if (x != y || x == 0) return x - y;
    }
}

int lstrcmpiA(LPCSTR a, LPCSTR b) {
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    for (;; ++a, ++b) {
        const int x = ascii_lower(static_cast<unsigned char>(*a));
        const int y = ascii_lower(static_cast<unsigned char>(*b));
        if (x != y || x == 0) return x - y;
    }
}

LPWSTR CharUpperW(LPWSTR s) {
    for (LPWSTR p = s; p && *p; ++p)
        if (*p >= L'a' && *p <= L'z') *p = static_cast<wchar_t>(*p - 32);
    return s;
}

BOOL CompareStringOrdinal(LPCWSTR a, int a_len, LPCWSTR b, int b_len, BOOL ignore_case) {
    const int na = a_len < 0 ? lstrlenW(a) : a_len;
    const int nb = b_len < 0 ? lstrlenW(b) : b_len;
    const int n = std::min(na, nb);
    for (int i = 0; i < n; i++) {
        int x = static_cast<int>(a[i]), y = static_cast<int>(b[i]);
        if (ignore_case) {
            x = ascii_lower(x);
            y = ascii_lower(y);
        }
        if (x != y) return 1;   // CSTR_LESS_THAN
    }
    if (na == nb) return 2;     // CSTR_EQUAL
    return na < nb ? 1 : 3;
}

DWORD FormatMessageW(DWORD, LPCVOID, DWORD msg_id, DWORD, LPWSTR out, DWORD cap, void*) {
    if (!out || cap == 0) return 0;
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "error %u", static_cast<unsigned>(msg_id));
    const int w = MultiByteToWideChar(CP_UTF8, 0, buf, n, out, static_cast<int>(cap) - 1);
    if (w > 0) out[w] = 0;
    return static_cast<DWORD>(w);
}

void OutputDebugStringA(LPCSTR s) {
    if (!s) return;
    std::fputs(s, stderr);
    std::fflush(stderr);
}

void OutputDebugStringW(LPCWSTR s) {
    if (!s) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string buf(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf.data(), n, nullptr, nullptr);
    std::fputs(buf.c_str(), stderr);
    std::fflush(stderr);
}

#if defined(__cplusplus)
std::string nxos_utf8(const char* s) { return s ? std::string(s) : std::string(); }
std::string nxos_utf8(const wchar_t* s) {
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

#endif  // !_WIN32
