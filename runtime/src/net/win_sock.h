// SPDX-License-Identifier: GPL-3.0-or-later
// Winsock start-up for every socket user in net/ (TCP client, UDP datagram path, LAN discovery, the embedded relay server): one WSAStartup for
// the process, never undone.
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
