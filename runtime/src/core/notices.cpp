// SPDX-License-Identifier: GPL-3.0-or-later
// Short messages over the game's picture (nxrt/notices.h): the list the overlay draws from.
#include "nxrt/notices.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace nxrt::notices {

namespace {

struct Entry {
    Notice n;
    double seconds = 0;
    double until_s = -1;   // < 0: not shown yet, its time starts at the first frame that draws it
};

struct List {
    std::mutex mu;
    std::vector<Entry> items;
    std::atomic<bool> any{false};
};
List& list() {
    static List l;
    return l;
}

constexpr size_t kMaxShown = 6;   // more sources at once: the oldest go first

}  // namespace

void post(const std::string& source, const std::string& text, double seconds) {
    List& l = list();
    std::lock_guard<std::mutex> lk(l.mu);
    std::erase_if(l.items, [&](const Entry& e) { return e.n.source == source; });
    if (text.empty() || !(seconds > 0)) {
        l.any.store(!l.items.empty(), std::memory_order_relaxed);
        return;
    }
    l.items.push_back(Entry{Notice{source, text}, std::min(seconds, 60.0), -1});
    if (l.items.size() > kMaxShown) l.items.erase(l.items.begin());
    l.any.store(true, std::memory_order_relaxed);
}

std::vector<Notice> shown(double now_s) {
    List& l = list();
    std::lock_guard<std::mutex> lk(l.mu);
    std::vector<Notice> out;
    for (Entry& e : l.items)
        if (e.until_s < 0) e.until_s = now_s + e.seconds;
    std::erase_if(l.items, [now_s](const Entry& e) { return now_s >= e.until_s; });
    for (const Entry& e : l.items) out.push_back(e.n);
    l.any.store(!l.items.empty(), std::memory_order_relaxed);
    return out;
}

bool any() { return list().any.load(std::memory_order_relaxed); }

}  // namespace nxrt::notices
