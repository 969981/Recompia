// SPDX-License-Identifier: GPL-3.0-or-later
// The host HTTP(S) client (http_client.h) on WinHTTP, in its synchronous mode on one worker thread per request. On Linux the game is this
// same Windows exe under Proton, whose WinHTTP carries the requests, so there is no second implementation.
#include "http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <thread>

#include "nxrt/log.h"
#include "nxrt/text.h"

#pragma comment(lib, "winhttp.lib")

namespace nxrt::http {

namespace {

constexpr const char* kTag = "http";

std::atomic<int> g_in_flight{0};

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// The parts of an allowed URL, or false: https, or http to localhost / 127.0.0.1 only; no user name or password in it.
struct Target {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool tls = true;
};
bool parse(const std::string& url, Target* t) {
    const std::wstring w = text::widen(url);
    URL_COMPONENTS uc = {sizeof uc};
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    uc.dwUserNameLength = uc.dwPasswordLength = static_cast<DWORD>(-1);
    if (w.empty() || !WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) return false;
    if (uc.dwUserNameLength || uc.dwPasswordLength || !uc.dwHostNameLength) return false;
    t->host.assign(uc.lpszHostName, uc.dwHostNameLength);
    t->path.assign(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) t->path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (t->path.empty()) t->path = L"/";
    t->port = uc.nPort;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) return t->tls = true;
    t->tls = false;
    return uc.nScheme == INTERNET_SCHEME_HTTP && (_wcsicmp(t->host.c_str(), L"localhost") == 0 || t->host == L"127.0.0.1");
}

// One session for the process (WinHTTP handles are safe across threads): no proxy sign-in, no redirects, TLS 1.2 or later.
HINTERNET session(const std::string& user_agent) {
    static HINTERNET s = [&] {
        const std::wstring ua = text::widen(user_agent.empty() ? std::string("nxrt") : user_agent);
        HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // before 8.1
        if (!h) {
            NXLOG_W(kTag, "WinHttpOpen failed (%lu): no host HTTP requests this run", GetLastError());
            return h;
        }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
        DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {   // a WinHTTP without TLS 1.3 refuses the flag
            tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
        }
        return h;
    }();
    return s;
}

// The request itself, on the worker: the status and the body, or 0 when there is no answer.
int run(const Request& r, const Target& t, std::string* out) {
    HINTERNET s = session(r.user_agent);
    if (!s) return 0;
    Handle con(WinHttpConnect(s, t.host.c_str(), t.port, 0));
    if (!con.h) return 0;
    const std::wstring verb = text::widen(r.method);
    Handle req(WinHttpOpenRequest(con.h, verb.c_str(), t.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  t.tls ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) return 0;
    const int to = r.step_timeout_ms;
    WinHttpSetTimeouts(req.h, to, to, to, to);
    DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off)) return 0;   // never send without these off
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;   // never the Windows account's credentials
    WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
    std::wstring headers;
    if (!r.body.empty()) headers = L"Content-Type: " + text::widen(r.content_type);
    void* body = r.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(r.body.data());
    const DWORD n = static_cast<DWORD>(r.body.size());
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1),
                            body, n, n, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return 0;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return 0;
    out->clear();
    for (;;) {
        char buf[16384];
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf, sizeof buf, &got)) return 0;
        if (got == 0) break;
        if (out->size() + got > r.max_answer) return 0;   // too long an answer counts as none: stop reading
        out->append(buf, got);
    }
    return static_cast<int>(status);
}

}  // namespace

bool start(Request r, DoneFn done) {
    if (!done || (r.method != "GET" && r.method != "POST")) return false;
    Target t;
    if (!parse(r.url, &t)) return false;
    if (g_in_flight.fetch_add(1) >= kMaxInFlight) {
        g_in_flight.fetch_sub(1);
        return false;
    }
    try {
        std::thread([r = std::move(r), t = std::move(t), done = std::move(done)] {
            std::string body;
            const int status = run(r, t, &body);
            if (!status) body.clear();
            g_in_flight.fetch_sub(1);
            done(status, std::move(body));
        }).detach();
    } catch (...) {   // no thread could be made
        g_in_flight.fetch_sub(1);
        return false;
    }
    return true;
}

}  // namespace nxrt::http
