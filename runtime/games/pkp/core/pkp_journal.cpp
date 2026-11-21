// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal (pkp_journal.h). Lines go through nxrt/log.h at Info (Warn for MISMATCH and warn()); the crash and freeze
// section is kept here: a ring of the last kRing records and the last step of every area, read with try_lock (diag_add_report's rule:
// the crashing or frozen thread may hold the lock).
#include "core/pkp_journal.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "nxrt/diag.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr size_t kRing = 64;
constexpr size_t kListLine = 1600;   // bytes of items per list line, under log.h's 2000-byte cut with the prefix and parts
constexpr const char* kTags[] = {"j.session", "j.sync", "j.quest", "j.talk", "j.event", "j.role",
                                 "j.item", "j.box", "j.travel", "j.discovery", "j.service", "j.interact"};
static_assert(sizeof kTags / sizeof kTags[0] == static_cast<size_t>(Area::Count), "one tag per area");

std::atomic<Who (*)()> g_who{nullptr};

struct Ring {
    std::mutex m;
    std::array<std::string, kRing> lines;
    size_t next = 0, total = 0;
    std::array<std::string, static_cast<size_t>(Area::Count)> last_step;
};
Ring& ring() {
    static Ring* r = new Ring;   // never destroyed: the crash handler may read it during static destruction
    return *r;
}

void keep(Area a, const std::string& line, bool step) {
    Ring& r = ring();
    std::lock_guard<std::mutex> lk(r.m);
    char t[32];
    std::snprintf(t, sizeof t, "%.3f ", log_elapsed());
    r.lines[r.next] = t + line;
    r.next = (r.next + 1) % kRing;
    ++r.total;
    if (step) r.last_step[static_cast<size_t>(a)] = t + line;
}

std::string report() {
    Ring& r = ring();
    std::unique_lock<std::mutex> lk(r.m, std::try_to_lock);
    if (!lk.owns_lock()) return "journal: (busy)\n";
    std::string out = "journal: the last step of each area (seconds since the log started, as the log's lines)\n";
    for (size_t i = 0; i < r.last_step.size(); ++i)
        if (!r.last_step[i].empty()) out += "  " + std::string(kTags[i]) + " " + r.last_step[i] + "\n";
    const size_t n = r.total < kRing ? r.total : kRing;
    char head[96];
    std::snprintf(head, sizeof head, "journal: the last %zu records of %zu, oldest first\n", n, r.total);
    out += head;
    for (size_t i = 0; i < n; ++i) out += "  " + r.lines[(r.next + kRing - n + i) % kRing] + "\n";
    return out;
}

std::string who_text() {
    Who w;
    if (auto fn = g_who.load(std::memory_order_acquire)) w = fn();
    char b[48];
    if (w.node < 0) std::snprintf(b, sizeof b, "%s", w.role ? w.role : "solo");
    else std::snprintf(b, sizeof b, "n%d %s", w.node, w.role ? w.role : "?");
    return b;
}

void emit(Area a, bool warn, bool step, const std::string& body) {
    const LogLevel lv = warn ? LogLevel::Warn : LogLevel::Info;
    const std::string line = who_text() + " " + body;
    keep(a, line, step);
    if (log_enabled(lv)) logf(lv, kTags[static_cast<size_t>(a)], "%s", line.c_str());
    if (step) log_flush();
}

}  // namespace

const char* area_name(Area a) { return a < Area::Count ? kTags[static_cast<size_t>(a)] + 2 : "?"; }

void set_who(Who (*fn)()) { g_who.store(fn, std::memory_order_release); }

Rec::Rec(Area a, const char* what) : area_(a < Area::Count ? a : Area::Session), what_(what ? what : "") {}

Rec::~Rec() { write(); }

void Rec::add(std::string& part, const char* f, va_list ap) {
    char b[1024];
    b[0] = 0;
    if (f) std::vsnprintf(b, sizeof b, f, ap);
    if (!part.empty()) part += "; ";
    part += b;
}

#define PKP_JOURNAL_PART(name, field)            \
    Rec& Rec::name(const char* f, ...) {         \
        va_list ap;                              \
        va_start(ap, f);                         \
        add(field, f, ap);                       \
        va_end(ap);                              \
        return *this;                            \
    }
PKP_JOURNAL_PART(in, in_)
PKP_JOURNAL_PART(out, out_)
PKP_JOURNAL_PART(expect, expect_)
PKP_JOURNAL_PART(note, note_)
#undef PKP_JOURNAL_PART

Rec& Rec::refused(const char* f, ...) {
    std::string why;
    va_list ap;
    va_start(ap, f);
    add(why, f, ap);
    va_end(ap);
    verdict_ = "REFUSED: " + why;
    return *this;
}

Rec& Rec::ok(bool matched) {
    if (verdict_.rfind("REFUSED", 0) == 0) return *this;   // a refusal is the verdict that counts
    verdict_ = matched ? "ok" : "MISMATCH";
    if (!matched) warn_ = true;
    return *this;
}

Rec& Rec::warn() {
    warn_ = true;
    return *this;
}

Rec& Rec::step() {
    step_ = true;
    return *this;
}

void Rec::write() {
    if (done_) return;
    done_ = true;
    std::string body = what_;
    if (!in_.empty()) body += " | in: " + in_;
    if (!out_.empty()) body += " | out: " + out_;
    if (!expect_.empty()) body += " | expect: " + expect_;
    if (!verdict_.empty()) body += " | " + verdict_;
    if (!note_.empty()) body += " | " + note_;
    emit(area_, warn_, step_, body);
}

void step(Area a, const char* what, const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    b[0] = 0;
    if (f) std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    emit(a < Area::Count ? a : Area::Session, false, true, std::string(what ? what : "") + " | " + b);
}

void list(Area a, const char* what, const std::vector<std::string>& items) {
    if (a >= Area::Count) a = Area::Session;
    const std::string name = what ? what : "";
    char head[64];
    std::snprintf(head, sizeof head, " (%zu items)", items.size());
    emit(a, false, false, name + head);
    if (items.empty()) return;
    std::vector<std::string> lines(1);
    for (const std::string& it : items) {
        if (!lines.back().empty() && lines.back().size() + it.size() + 2 > kListLine) lines.emplace_back();
        if (!lines.back().empty()) lines.back() += ", ";
        lines.back() += it.size() > kListLine ? it.substr(0, kListLine) + "..." : it;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        char idx[48];
        std::snprintf(idx, sizeof idx, " [%zu/%zu]: ", i + 1, lines.size());
        emit(a, false, false, name + idx + lines[i]);
    }
}

void install() {
    static std::once_flag once;
    std::call_once(once, [] { diag_add_report(&report); });
}

std::string hex_bytes(const void* p, size_t n, size_t max) {
    std::string s;
    const auto* b = static_cast<const uint8_t*>(p);
    const size_t m = n < max ? n : max;
    char h[4];
    for (size_t i = 0; i < m; ++i) {
        std::snprintf(h, sizeof h, i ? " %02x" : "%02x", b[i]);
        s += h;
    }
    if (n > m) s += " +" + std::to_string(n - m);
    return s;
}

std::string fmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    b[0] = 0;
    if (f) std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

}  // namespace nxrt::games::pkp::journal
