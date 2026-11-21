// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's lines in the game's own message log (pkp_coop_internal.h). What one player's event shows only on
// that player's screen (an unlock, a skill, a quest the others' story moved on with) is told to the others in a line of the game's own
// log, the one its "busy" refusal uses, with the name of the player who did it. Every member sends its player's name (MemberName) when
// it joins and when another member joins.
//
// The game, 2.0.1:
//   0xa8fba0(w0 slot, x1 text, w2 1)  a line in the message log. text: +0 u32 kind, +8 u64 (the text template's word at
//                                     [main+0x3483e98] with the text id in its low 16 bits), +0x10 arguments, +0x18 their count,
//                                     +0x20 capacity 6; 6 arguments of 0x88 bytes follow (+0 u32 type, -1 unused). The kind picks
//                                     the text bank: kind 0 id 0xa8 is "<string>", as 0xf35f20 builds it for a widget (kind 0x44, the
//                                     bank of the game's own log lines such as the busy one, 0x318274, has no plain string; kind 0x1c
//                                     id 0xa8 is a line of another bank). Argument type 7 carries the string inline at +8 (up to 0x80 bytes).
//   slot: the player's event slot, as the talk start gives it (pkp_coop_talk.cpp passes it on); 0 until one was seen.
// Queued lines wait until the player can walk (no event or menu running), and come at least kGap apart, so none is lost under a
// cutscene and two do not cover each other.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::notice {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kShowLine = 0xa8fba0, kTextTemplate = 0x3483e98;
constexpr uint32_t kTextPlain = 0, kTextString = 0xa8, kArgString = 7, kArgNone = 0xffffffffu;
constexpr size_t kTextArgs = 0x28, kArgSize = 0x88, kArgCount = 6, kArgText = 0x80;
constexpr double kGap = 2.5;              // seconds between two queued lines
constexpr size_t kQueueMax = 8;           // older lines are dropped past this many
constexpr size_t kNodes = 8;

struct State {
    alignas(16) uint8_t text[kTextArgs + kArgCount * kArgSize] = {};   // the line's text, guest-readable
    std::deque<std::string> queue;
    double shown_at = -1e9;
    uint8_t slot = 0;
    bool send_name = true;
    std::string names[kNodes];
};
State& st() {
    static State s;
    return s;
}

void send_name() {
    State& s = st();
    if (!link::in_session()) return;
    std::string name = catchup::player_name();
    if (name.empty()) return;   // a new save before its name entry: sent once there is one
    s.send_name = false;
    if (name.size() > wire::kNameMax) name.resize(wire::kNameMax);
    link::share(wire::Type::MemberName, wire::Writer(link::self())
                                            .u8(static_cast<uint8_t>(name.size()))
                                            .bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size())
                                            .take());
}

}  // namespace

void line(::Ctx* c, uint8_t slot, const char* text) {
    State& s = st();
    std::memset(s.text, 0, sizeof s.text);
    const uint64_t t = reinterpret_cast<uint64_t>(s.text);
    const uint64_t tmpl = guest::rd<uint64_t>(base() + kTextTemplate);
    const uint64_t word = guest::ok(tmpl, 8) ? guest::rd<uint64_t>(tmpl) : 0;
    guest::wr<uint32_t>(t, kTextPlain);
    guest::wr<uint64_t>(t + 8, (word & ~uint64_t{0xffff}) | kTextString);
    guest::wr<uint64_t>(t + 0x10, t + kTextArgs);
    guest::wr<uint64_t>(t + 0x18, 1);
    guest::wr<uint64_t>(t + 0x20, kArgCount);
    for (size_t i = 0; i < kArgCount; ++i) guest::wr<uint32_t>(t + kTextArgs + i * kArgSize, kArgNone);
    guest::wr<uint32_t>(t + kTextArgs, kArgString);
    size_t n = std::min(std::strlen(text), kArgText - 1);
    if (n < std::strlen(text))
        while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;   // cut at a whole UTF-8 character (translated lines)
    std::memcpy(s.text + kTextArgs + 8, text, n);
    guest::call(c, base() + kShowLine, slot, t, 1);
}

void post(const std::string& text) {
    State& s = st();
    if (s.queue.size() >= kQueueMax) s.queue.pop_front();
    s.queue.push_back(text);
    NXLOG_I(kTag, "Co-op notice: \"%s\"", text.c_str());
}

std::string name_of(uint8_t node) {
    const State& s = st();
    return node < kNodes && !s.names[node].empty() ? s.names[node] : std::string(strings::get(base(), strings::Str::CoopTeammate));
}

void set_slot(uint8_t slot) { st().slot = slot; }

void on_name_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    const uint8_t n = r.u8();
    if (!r.ok() || n == 0 || n > wire::kNameMax || origin >= kNodes) return;
    char buf[wire::kNameMax];
    r.bytes(reinterpret_cast<uint8_t*>(buf), n);
    if (!r.ok()) return;
    std::string name(buf, n);
    for (char& ch : name)
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    if (st().names[origin] != name) NXLOG_I(kTag, "Co-op notice: node %u is \"%s\"", origin, name.c_str());
    st().names[origin] = std::move(name);
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (s.send_name) send_name();
    if (s.queue.empty() || !session::can_walk(in_game)) return;
    const double now = qpc::seconds();
    if (now - s.shown_at < kGap) return;
    s.shown_at = now;
    line(c, s.slot, s.queue.front().c_str());
    s.queue.pop_front();
}

void on_session_change() {
    State& s = st();
    s.send_name = true;
    for (std::string& n : s.names) n.clear();
}

void on_member_joined() { st().send_name = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::notice
