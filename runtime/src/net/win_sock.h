// SPDX-License-Identifier: GPL-3.0-or-later
// Winsock start-up for the long-lived sockets of net/ (the TCP client sockets and the embedded relay server): one WSAStartup for the process,
// never undone. Calls that open a short-lived socket (UDP datagram path, LAN discovery) pair their own WSAStartup / WSACleanup instead.
// A runtime-wide header could replace this file when the other directories' Winsock start-ups are unified.
#pragma once
#include <winsock2.h>

#include <mutex>

namespace nxrt::vnet {

inline bool ensure_winsock() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wd;
        ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    });
    return ok;
}

}  // namespace nxrt::vnet
