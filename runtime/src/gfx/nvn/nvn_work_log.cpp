// SPDX-License-Identifier: GPL-3.0-or-later
// The report text of one submission's work log (nvn_work_log.h). Cold path: runs once on a device loss, or in a freeze dump.
#include "nvn_work_log.h"

#include <cstdio>
#include <vector>

namespace nxrt::nvn::be::gpu {
namespace {

std::string key_text(const char* k) {
    if (!k[0]) return "-";
    return std::string(k, strnlen(k, 16));
}

}  // namespace

std::string work_log_text(const WorkLog& log, uint64_t serial, const char* indent, uint32_t max_draw_lines) {
    const uint32_t nd = log.ndisp, nr = log.ndraw;
    const uint32_t kd = std::min(nd, WorkLog::kDispatches), kr = std::min(nr, WorkLog::kDraws);
    char b[256];
    std::snprintf(b, sizeof b, "%ssubmission %llu: %u dispatch%s, %u draw%s", indent, static_cast<unsigned long long>(serial), nd,
                  nd == 1 ? "" : "es", nr, nr == 1 ? "" : "s");
    std::string out = b;
    if (nd > kd || nr > kr) {
        std::snprintf(b, sizeof b, " (only the last %u dispatches and %u draws are kept)", kd, kr);
        out += b;
    }
    out += '\n';
    // Distinct entries in the order first recorded (oldest kept entry first), with how often each was recorded.
    struct Row { uint32_t at, n; };
    std::vector<Row> rows;
    for (uint32_t i = nd - kd; i < nd; ++i) {
        const WorkLog::Dispatch& e = log.disp[i % WorkLog::kDispatches];
        bool seen = false;
        for (Row& r : rows) {
            const WorkLog::Dispatch& o = log.disp[r.at % WorkLog::kDispatches];
            if (!std::memcmp(o.cs, e.cs, 16) && o.x == e.x && o.y == e.y && o.z == e.z && o.indirect == e.indirect) { ++r.n; seen = true; break; }
        }
        if (!seen) rows.push_back({i, 1});
    }
    for (size_t j = 0; j < rows.size(); ++j) {
        const WorkLog::Dispatch& e = log.disp[rows[j].at % WorkLog::kDispatches];
        if (e.indirect)
            std::snprintf(b, sizeof b, "%s  dispatch cs %s, indirect (arguments at guest %#llx)", indent, key_text(e.cs).c_str(),
                          static_cast<unsigned long long>(e.x | static_cast<uint64_t>(e.y) << 32));
        else std::snprintf(b, sizeof b, "%s  dispatch cs %s, %ux%ux%u groups", indent, key_text(e.cs).c_str(), e.x, e.y, e.z);
        out += b;
        if (rows[j].n > 1) out += " x" + std::to_string(rows[j].n);
        out += '\n';
    }
    rows.clear();
    for (uint32_t i = nr - kr; i < nr; ++i) {
        const WorkLog::Draw& e = log.draw[i % WorkLog::kDraws];
        bool seen = false;
        for (Row& r : rows) {
            const WorkLog::Draw& o = log.draw[r.at % WorkLog::kDraws];
            if (!std::memcmp(o.vs, e.vs, 16) && !std::memcmp(o.fs, e.fs, 16)) { ++r.n; seen = true; break; }
        }
        if (!seen) rows.push_back({i, 1});
    }
    for (size_t j = 0; j < rows.size() && j < max_draw_lines; ++j) {
        const WorkLog::Draw& e = log.draw[rows[j].at % WorkLog::kDraws];
        std::snprintf(b, sizeof b, "%s  draw vs %s fs %s", indent, key_text(e.vs).c_str(), key_text(e.fs).c_str());
        out += b;
        if (rows[j].n > 1) out += " x" + std::to_string(rows[j].n);
        out += '\n';
    }
    if (rows.size() > max_draw_lines) out += std::string(indent) + "  ... " + std::to_string(rows.size() - max_draw_lines) + " more distinct draws\n";
    return out;
}

}  // namespace nxrt::nvn::be::gpu
