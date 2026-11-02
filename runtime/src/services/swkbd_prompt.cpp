// SPDX-License-Identifier: GPL-3.0-or-later
// The software keyboard's text prompt on the PC (am.cpp, swkbdui in services_api.h): a small native window with the keyboard's header,
// guide text and an edit box holding the initial text. Enter or OK answers the text, Escape or closing the window cancels. It runs on its
// own host thread, so it does not depend on the game presenting frames while the keyboard is up.
#include <windows.h>

#include <string>

#include "nxrt/log.h"
#include "nxrt/services_api.h"

namespace nxrt::services::swkbdui {

namespace {

constexpr int kIdEdit = 100;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;
constexpr wchar_t kClass[] = L"nxrt_swkbd_prompt";

struct PromptState {
    HWND edit = nullptr;
    bool done = false;
    bool ok = false;
    std::wstring text;
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void finish(HWND hwnd, PromptState* ps, bool ok) {
    if (ps->done) return;
    ps->ok = ok;
    if (ok && ps->edit) {
        const int n = GetWindowTextLengthW(ps->edit);
        ps->text.assign(static_cast<size_t>(n) + 1, L'\0');
        GetWindowTextW(ps->edit, ps->text.data(), n + 1);
        ps->text.resize(static_cast<size_t>(n));
    }
    ps->done = true;
    DestroyWindow(hwnd);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ps = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND:
            if (ps && (LOWORD(wp) == kIdOk || LOWORD(wp) == kIdCancel)) finish(hwnd, ps, LOWORD(wp) == kIdOk);
            return 0;
        case WM_CLOSE:
            if (ps) finish(hwnd, ps, false);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

bool win32_prompt(const Request& r, std::string* text) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    std::string label = r.header;
    if (!r.sub.empty()) label += (label.empty() ? "" : "\n") + r.sub;
    if (!r.guide.empty()) label += (label.empty() ? "" : "\n") + r.guide;
    if (label.empty()) label = "Enter text";
    const int W = 460, H = 190;
    const int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kClass, widen(r.title.empty() ? "Software keyboard" : r.title).c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, (sx - W) / 2, (sy - H) / 2, W, H, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;
    PromptState ps;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ps));
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND lab = CreateWindowExW(0, L"STATIC", widen(label).c_str(), WS_CHILD | WS_VISIBLE, 12, 10, W - 36, 64, hwnd, nullptr, inst, nullptr);
    ps.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", widen(r.initial).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                              (r.password ? ES_PASSWORD : 0), 12, 80, W - 36, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEdit)), inst, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", widen(r.ok_text.empty() ? "OK" : r.ok_text).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              W - 200, 116, 80, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOk)), inst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - 110, 116, 80, 26, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), inst, nullptr);
    for (HWND c : {lab, ps.edit, ok, cancel})
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (r.max_len > 0) SendMessageW(ps.edit, EM_LIMITTEXT, r.max_len, 0);
    SendMessageW(ps.edit, EM_SETSEL, 0, -1);
    SetForegroundWindow(hwnd);
    SetFocus(ps.edit);
    MSG m;
    while (!ps.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) { finish(hwnd, &ps, true); continue; }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { finish(hwnd, &ps, false); continue; }
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (ps.ok) *text = narrow(ps.text);
    return ps.ok;
}

}  // namespace nxrt::services::swkbdui
