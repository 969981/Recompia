// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt/tt_coop.h: the co-op channel's recorder and injector and the shared-state lines. The channel calls on_sent / on_received /
// drain_injected (services/ldn_channel.cpp), the script runner run_step, the game layer state_frame. Each part keeps its own lock: the
// channel is used from any thread, the runner runs at the frame boundary and the drain on the game layer's thread.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/text.h"
#include "nxrt/tt.h"
#include "nxrt/tt_coop.h"
#include "../gfx/nvn/nvn_fnv.h"

namespace nxrt::tt::coop {

namespace {

using text::lower;
using text::trim;

constexpr const char* kTag = "tt";
constexpr const char* kLogHeader = "# nxrt coop channel log 1: ms drain dir node type payload";

struct Config {
    std::string record, state;
    int64_t state_every = 0;
};
const Config& cfg() {
    static const Config c = [] {
        Config k;
        k.record = config_get_str("test", "coop_record", "");
        k.state = config_get_str("test", "coop_state", "");
        k.state_every = std::max<int64_t>(0, config_get_int("test", "coop_state_every", 0));
        return k;
    }();
    return c;
}

double now_ms() { return qpc::seconds() * 1000.0; }

std::string to_hex(const uint8_t* p, size_t n) {
    if (!n) return "-";
    static const char kDigits[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = kDigits[p[i] >> 4];
        s[i * 2 + 1] = kDigits[p[i] & 15];
    }
    return s;
}

bool from_hex(const std::string& s, std::vector<uint8_t>* out) {
    out->clear();
    if (s == "-") return true;
    if (s.size() % 2) return false;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

bool to_int(const std::string& s, int64_t* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    *v = std::strtoll(s.c_str(), &end, 0);
    return end && *end == 0;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// ---- the recorder ----
struct Recorder {
    std::mutex mu;
    FILE* f = nullptr;
    bool tried = false;
    double t0 = 0;
};
Recorder& rec() {
    static Recorder r;
    return r;
}

std::atomic<uint64_t>& drains() {
    static std::atomic<uint64_t> n{0};
    return n;
}

void record(const char* dir, int node, uint32_t type, const uint8_t* data, size_t size) {
    if (cfg().record.empty()) return;
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (!r.f && !r.tried) {
        r.tried = true;
        r.f = std::fopen(cfg().record.c_str(), "wb");
        if (!r.f) {
            NXLOG_E(kTag, "coop: cannot open the message log %s", cfg().record.c_str());
            return;
        }
        r.t0 = now_ms();
        std::fprintf(r.f, "%s\n", kLogHeader);
        NXLOG_I(kTag, "coop: recording the channel's messages to %s", cfg().record.c_str());
    }
    if (!r.f) return;
    std::fprintf(r.f, "%.0f %llu %s %d %u %s\n", now_ms() - r.t0, static_cast<unsigned long long>(drains().load()), dir, node, type,
                 to_hex(data, size).c_str());
    std::fflush(r.f);
}

// ---- the injector ----
struct Message {
    double ms = 0;   // the recorded time (pace=rec); then the host time it is due
    uint64_t drain = 0;   // due at this drain count
    int node = 0;
    uint32_t type = 0;
    std::vector<uint8_t> data;
};
struct Injector {
    std::mutex mu;
    std::deque<Message> queue;
};
Injector& inj() {
    static Injector s;
    return s;
}

struct Options {
    std::string dir = "in";
    bool pace = false, have_node = false;
    int node = 0;
    int64_t delay = 0, dup = 0, drop = 0, reorder = 0, seed = 1;
};

bool parse_options(const std::vector<std::string>& w, size_t first, bool file, Options* o, std::string* err) {
    o->pace = file;
    for (size_t i = first; i < w.size(); ++i) {
        const size_t eq = w[i].find('=');
        const std::string key = lower(w[i].substr(0, eq)), val = eq == std::string::npos ? "" : w[i].substr(eq + 1);
        int64_t v = 0;
        const bool num = to_int(val, &v);
        if (key == "dir" && (val == "in" || val == "loop" || val == "out" || val == "inj" || val == "all")) o->dir = val;
        else if (key == "pace" && (val == "rec" || val == "none")) o->pace = val == "rec";
        else if (key == "node" && num) o->node = static_cast<int>(v), o->have_node = true;
        else if (key == "delay" && num && v >= 0) o->delay = v;
        else if (key == "dup" && num && v >= 0) o->dup = v;
        else if (key == "drop" && num && v >= 0) o->drop = v;
        else if (key == "reorder" && num && v >= 0) o->reorder = v;
        else if (key == "seed" && num) o->seed = v;
        else {
            *err = "bad option '" + w[i] + "' (dir= pace= node= delay= dup= drop= reorder= seed=)";
            return false;
        }
    }
    return true;
}

bool read_log(const std::string& path, const std::string& dir, std::vector<Message>* out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *err = "cannot read " + path;
        return false;
    }
    int line_no = 0;
    for (std::string line; std::getline(in, line);) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = words(line);
        int64_t node = 0, type = 0;
        Message m;
        if (f.size() != 6 || !to_int(f[3], &node) || !to_int(f[4], &type) || type < 0 || !from_hex(f[5], &m.data)) {
            *err = path + ":" + std::to_string(line_no) + ": not a message line (ms drain dir node type payload)";
            return false;
        }
        if (dir != "all" && f[2] != dir) continue;
        m.ms = std::strtod(f[0].c_str(), nullptr);
        m.node = static_cast<int>(node);
        m.type = static_cast<uint32_t>(type);
        out->push_back(std::move(m));
    }
    return true;
}

uint64_t splitmix(uint64_t* s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Applies the faults and the timing, then queues: drop, dup and reorder count over the step's messages, so a run is repeatable.
void queue(std::vector<Message>&& in, const Options& o) {
    std::vector<Message> ms;
    for (size_t i = 0; i < in.size(); ++i) {
        const size_t k = i + 1;
        if (o.drop && k % o.drop == 0) continue;
        if (o.have_node) in[i].node = o.node;
        if (o.dup && k % o.dup == 0) ms.push_back(in[i]);
        ms.push_back(std::move(in[i]));
    }
    std::vector<double> times;
    for (const Message& m : ms) times.push_back(m.ms);   // the time slots stay in order; reorder moves the messages between them
    if (o.reorder > 1) {
        uint64_t seed = static_cast<uint64_t>(o.seed);
        for (size_t at = 0; at < ms.size(); at += static_cast<size_t>(o.reorder)) {
            const size_t end = std::min(ms.size(), at + static_cast<size_t>(o.reorder));
            for (size_t i = end - 1; i > at; --i) std::swap(ms[i], ms[at + splitmix(&seed) % (i - at + 1)]);
        }
    }
    const double now = now_ms(), first = times.empty() ? 0 : times.front();
    Injector& s = inj();
    std::lock_guard<std::mutex> lk(s.mu);
    const uint64_t due = drains().load() + 1 + static_cast<uint64_t>(o.delay);
    for (size_t i = 0; i < ms.size(); ++i) {
        ms[i].ms = o.pace ? now + (times[i] - first) : now;
        ms[i].drain = due;
        s.queue.push_back(std::move(ms[i]));
    }
}

// ---- the state lines ----
struct StateLog {
    std::mutex mu;
    std::vector<std::string> labels;   // @coop_state steps waiting for the next game frame
    uint64_t frame = 0, last_all = 0;
    bool have_last = false, tried = false;
    FILE* f = nullptr;
};
StateLog& states() {
    static StateLog s;
    return s;
}

uint64_t hash_text(uint64_t h, const std::string& t) {
    for (const char c : t) h = nvn::be::fnv_step(h, static_cast<uint8_t>(c));
    return h;
}

void write_state(StateLog& s, const std::string& label, const std::vector<StateSection>& parts, uint64_t all) {
    if (!s.f && !s.tried) {
        s.tried = true;
        std::string path = cfg().state;
        if (path.empty()) path = (std::filesystem::path(out_dir().empty() ? std::string("testout") : out_dir()) / "coop_state.txt").string();
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        s.f = std::fopen(path.c_str(), "wb");
        if (!s.f) NXLOG_E(kTag, "coop: cannot open the state file %s", path.c_str());
        else NXLOG_I(kTag, "coop: shared-state lines go to %s", path.c_str());
    }
    std::string line = "state " + std::to_string(s.frame) + " " + label;
    char hex[24];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(all));
    line += std::string(" all=") + hex;
    for (const StateSection& p : parts) {
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash_text(nvn::be::kFnvBasis, p.text)));
        line += " " + p.name + "=" + hex;
    }
    NXLOG_I(kTag, "coop %s", line.c_str());
    if (!s.f) return;
    std::fprintf(s.f, "%s\n", line.c_str());
    for (const StateSection& p : parts) std::fprintf(s.f, "  %s %s\n", p.name.c_str(), p.summary.c_str());
    std::fflush(s.f);
}

bool check_or_run(const std::string& text, bool run, std::string* err) {
    const std::vector<std::string> w = words(text);
    const std::string verb = w.empty() ? "" : lower(w[0]);
    if (verb == "coop_state") {
        if (!run) return true;
        StateLog& s = states();
        std::lock_guard<std::mutex> lk(s.mu);
        const std::string label = trim(text.substr(w[0].size()));
        s.labels.push_back(label.empty() ? "-" : label);
        return true;
    }
    if (verb == "coop_msg") {
        int64_t type = 0, node = 0;
        Message m;
        if (w.size() < 4 || !to_int(w[1], &type) || type < 0 || !to_int(w[2], &node) || !from_hex(w[3], &m.data)) {
            *err = "usage: @coop_msg <type> <node> <payload hex> [options]";
            return false;
        }
        Options o;
        if (!parse_options(w, 4, false, &o, err)) return false;
        if (!run) return true;
        m.type = static_cast<uint32_t>(type);
        m.node = static_cast<int>(node);
        std::vector<Message> one;
        one.push_back(std::move(m));
        queue(std::move(one), o);
        return true;
    }
    if (verb == "coop_inject") {
        Options o;
        if (w.size() < 2) {
            *err = "usage: @coop_inject <file> [options]";
            return false;
        }
        if (!parse_options(w, 2, true, &o, err)) return false;
        if (!run) return true;
        std::vector<Message> ms;
        if (!read_log(w[1], o.dir, &ms, err)) return false;
        NXLOG_I(kTag, "coop: injecting %zu message(s) from %s", ms.size(), w[1].c_str());
        queue(std::move(ms), o);
        return true;
    }
    *err = "unknown step @" + (w.empty() ? std::string() : w[0]) + " (@coop_msg @coop_inject @coop_state)";
    return false;
}

}  // namespace

void on_sent(int node, uint32_t type, const void* data, size_t size) { record("out", node, type, static_cast<const uint8_t*>(data), size); }

void on_received(int from, bool loopback, uint32_t type, const uint8_t* data, size_t size) {
    record(loopback ? "loop" : "in", from, type, data, size);
}

size_t drain_injected(DeliverFn deliver) {
    const uint64_t drain = drains().fetch_add(1) + 1;
    std::vector<Message> due;
    {
        Injector& s = inj();
        std::lock_guard<std::mutex> lk(s.mu);
        const double now = now_ms();
        for (auto it = s.queue.begin(); it != s.queue.end();) {   // every due one, in queue order: a delayed message lets later ones pass
            if (it->drain <= drain && it->ms <= now) {
                due.push_back(std::move(*it));
                it = s.queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Message& m : due) {
        record("inj", m.node, m.type, m.data.data(), m.data.size());
        deliver(m.node, m.type, m.data.data(), m.data.size());
    }
    return due.size();
}

void state_frame(StateFillFn fill) {
    StateLog& s = states();
    std::lock_guard<std::mutex> lk(s.mu);
    ++s.frame;
    const bool periodic = cfg().state_every > 0 && s.frame % static_cast<uint64_t>(cfg().state_every) == 0;
    if (s.labels.empty() && !periodic) return;
    std::vector<StateSection> parts;
    fill(&parts);
    uint64_t all = nvn::be::kFnvBasis;
    for (const StateSection& p : parts) all = hash_text(hash_text(all, p.name), p.text);
    for (const std::string& label : s.labels) write_state(s, label, parts, all);
    if (s.labels.empty() && (!s.have_last || all != s.last_all)) write_state(s, "every", parts, all);
    s.labels.clear();
    s.last_all = all;
    s.have_last = true;
}

bool check_step(const std::string& text, std::string* err) { return check_or_run(text, false, err); }

bool run_step(const std::string& text, std::string* err) { return check_or_run(text, true, err); }

}  // namespace nxrt::tt::coop
