// SPDX-License-Identifier: GPL-3.0-or-later
// The developer A/B switch (nxrt/ab_switch.h).
#include "nxrt/ab_switch.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::diag {
namespace {
struct Cfg {
    std::string name, file;
};
const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg r;
        r.file = config_get_str("diag", "ab_file", "");
        r.name = config_get_str("diag", "ab_name", "");
        if (!r.file.empty() && !r.name.empty()) NXLOG_I("ab", "A/B switch '%s' from %s ([diag] ab_name, ab_file: write 1 or 0)", r.name.c_str(), r.file.c_str());
        return r;
    }();
    return c;
}
std::atomic<int> g_state{-1};          // -1: no answer from the file (def), 0 off, 1 on
std::atomic<uint64_t> g_next_ns{0};
std::mutex g_read_mu;
}  // namespace

bool ab(const char* name, bool def) {
    const Cfg& c = cfg();
    if (c.file.empty() || c.name != name) return def;
    const uint64_t now = qpc::now_ns();
    if (now >= g_next_ns.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(g_read_mu, std::try_to_lock);
        if (lk.owns_lock() && now >= g_next_ns.load(std::memory_order_relaxed)) {
            g_next_ns.store(now + 250000000ull, std::memory_order_relaxed);
            int v = -1;
            if (std::FILE* f = std::fopen(c.file.c_str(), "rb")) {
                const int ch = std::fgetc(f);
                std::fclose(f);
                v = ch == '1' ? 1 : ch == '0' ? 0 : -1;
            }
            if (g_state.exchange(v, std::memory_order_relaxed) != v) NXLOG_I("ab", "ab: %s %s", c.name.c_str(), v == 1 ? "on" : v == 0 ? "off" : "default");
        }
    }
    const int s = g_state.load(std::memory_order_relaxed);
    return s < 0 ? def : s == 1;
}

}  // namespace nxrt::diag
