// SPDX-License-Identifier: GPL-3.0-or-later
// Frame trace and debug aids of the replay: the [nvn] trace_* state and arming, draw snapshots, skip_fs_file, target dumps.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
bool g_trace = false;
std::vector<TraceSnap> g_trace_snaps;
static std::vector<int> g_trace_snap_draws;   // [nvn] trace_snap: draw numbers after which the targets are copied
static bool g_trace_snap_read = false;
static std::vector<std::string> g_trace_snap_fs;
static bool g_trace_snap_sampled = false;           // [nvn] trace_snap_sampled = 1: a snapshot draw also copies the textures that draw samples
std::vector<Texture*> g_trace_sampled;       // textures sampled by the last traced draw
std::vector<Ref<Texture>> g_trace_targets;   // the colour and depth targets drawn to in the traced frame, in order of first use
int64_t g_trace_frame = -2;
std::vector<int64_t> g_trace_extra;   // more frames to trace
// [nvn] trace_after_fs = "67d504": the frame after the first one that draws with a fragment shader of this hash prefix is traced
// (a frame number is not stable from run to run: the load times differ).
std::string g_trigger_fs;

// Debug: [nvn] skip_fs_file = FILE (env NXRT_NVN_SKIP_FS_FILE). FILE holds fragment shader hash prefixes separated by blanks; draws whose fragment
// shader starts with one of them are not drawn. The file is read again twice a second, so it can change while the game runs.
static std::vector<std::string> g_skip_fs;
int g_draw_idx = 0;   // draws begun in this frame
bool skip_fs_draw(const std::string& hash) {
    static const std::string file = nxrt::config_get_str("nvn", "skip_fs_file", "");
    if (file.empty()) return false;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || now - last > std::chrono::milliseconds(500)) {
        last = now;
        g_skip_fs.clear();
        std::ifstream f(file);
        std::string w;
        while (f >> w) g_skip_fs.push_back(w);
    }
    for (const std::string& p : g_skip_fs) {
        if (!p.empty() && p[0] == '#') {   // "#198" or "#198-210": the draw numbers of the frame (the first draw is #1, as in the trace)
            const int a = std::atoi(p.c_str() + 1);
            const size_t dash = p.find('-');
            const int b = dash == std::string::npos ? a : std::atoi(p.c_str() + dash + 1);
            if (g_draw_idx >= a && g_draw_idx <= b) return true;
        } else if (hash.compare(0, p.size(), p) == 0) return true;
    }
    return false;
}
bool g_trigger_read = false, g_trigger_seen = false, g_trigger_done = false;
int64_t g_trigger_present = -1;
int g_trace_n = 0;
std::atomic<bool> g_trace_req{false};   // backend_trace_next_frame(): trace the first whole frame after this flag is seen

const char* tex_desc(const Texture* t, char* buf, size_t n) {
    if (!t) { std::snprintf(buf, n, "-"); return buf; }
    std::snprintf(buf, n, "t%u:%s %dx%dx%d L%u/%u%s%s", t->id, t->fmt ? t->fmt->name : "?", t->b.width, t->b.height, t->b.depth, t->levels, t->layers,
                  t->b.samples > 1 ? " ms" : "", t->b.target == NVN_TEXTURE_TARGET_3D ? " 3D" : "");
    return buf;
}

// Percentage of the texture's storage (in the game's memory, first 16 MiB) that holds a non-zero 8-byte word; -1 without storage. Trace only.
int storage_nonzero_pct(const Texture* t) {
    if (!t || !t->mem || !t->storage_size) return -1;
    const uint64_t n = std::min<uint64_t>(t->storage_size, 16ull << 20) / 8;
    if (!n) return -1;
    uint64_t nz = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t w;
        std::memcpy(&w, t->mem + i * 8, 8);
        nz += w != 0;
    }
    return static_cast<int>(nz * 100 / n);
}
// [nvn] trace_min_sec = 250: trace_after_fs only starts that many seconds after the first draw (the scene to look at comes late).
bool trigger_time_ok() {
    static const auto start = std::chrono::steady_clock::now();
    static const double min_sec = static_cast<double>(nxrt::config_get_int("nvn", "trace_min_sec", 0));
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= min_sec;
}

// [nvn] trace_snap = "253,300": after those draws of the traced frame the pass is closed and the colour targets and the depth target are
// copied (png after the frame: snap_d<draw>_t<id>_<format>.png).
void trace_snap_after_draw() {
    if (!g_trace) return;
    if (!g_trace_snap_read) {
        g_trace_snap_read = true;
        g_trace_snap_sampled = nxrt::config_get_int("nvn", "trace_snap_sampled", 0) != 0;
        {   // [nvn] trace_snap_fs = "f77f9d,e72811": every draw whose fragment shader hash starts with one of these
            const std::string fl = nxrt::config_get_str("nvn", "trace_snap_fs", "");
            size_t k = 0;
            while (k < fl.size()) {
                size_t j = fl.find(',', k);
                if (j == std::string::npos) j = fl.size();
                if (j > k) g_trace_snap_fs.push_back(fl.substr(k, j - k));
                k = j + 1;
            }
        }
        const std::string list = nxrt::config_get_str("nvn", "trace_snap", "");
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            if (j > i) g_trace_snap_draws.push_back(std::atoi(list.substr(i, j - i).c_str()));
            i = j + 1;
        }
    }
    bool hit = false;
    for (int d : g_trace_snap_draws) hit |= d == g_trace_n;
    if (!hit && S.stage[kFragment] && !g_trace_snap_fs.empty()) {
        for (const std::string& pre : g_trace_snap_fs) hit |= S.stage[kFragment]->hash.compare(0, pre.size(), pre) == 0;
    }
    if (!hit) return;
    end_pass();
    auto take = [&](Texture* t) {
        if (!t) return;
        const int idx = snapshot_record(t, *F);
        if (idx < 0) return;
        char buf[96];
        g_trace_snaps.push_back({idx, g_trace_n, std::string("snap_d") + std::to_string(g_trace_n) + "_t" + std::to_string(t->id) + "_" + (t->fmt ? t->fmt->name : "x") + ".png", tex_desc(t, buf, sizeof buf)});
    };
    for (uint32_t i = 0; i < S.ncolor; ++i) take(S.color_tex[i].get());
    take(S.depth_tex.get());
    if (g_trace_snap_sampled) {
        for (Texture* t : g_trace_sampled) take(t);
    }
}

}  // namespace replay

void replay_request_trace() { g_trace_req.store(true); }

void trace_dump_targets() {
    for (const TraceSnap& sn : g_trace_snaps) {
        std::string stats;
        const bool ok = snapshot_write(sn.index, sn.name, &stats);
        NXLOG_I("nvn.trace", "snapshot after draw %d: %s -> %s: %s", sn.draw, sn.desc.c_str(), ok ? sn.name.c_str() : "(not written)", ok ? stats.c_str() : "");
    }
    g_trace_snaps.clear();
    if (g_trace_targets.empty()) return;
    std::vector<Ref<Texture>> list;
    list.swap(g_trace_targets);
    for (const Ref<Texture>& t : list) {
        char buf[96];
        std::string stats;
        const std::string png = std::string("trace_t") + std::to_string(t->id) + "_" + (t->fmt ? t->fmt->name : "x") + ".png";
        const bool ok = dump_texture(t.get(), png, &stats);
        NXLOG_I("nvn.trace", "target %s -> %s: %s", tex_desc(t.get(), buf, sizeof buf), ok ? png.c_str() : "(not dumped)", ok ? stats.c_str() : "");
    }
}

}  // namespace nxrt::nvn::be::gpu
