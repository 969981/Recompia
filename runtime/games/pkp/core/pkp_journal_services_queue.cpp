// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's look at Link Play's generic request queue (pkp_journal_services_internal.h). Every co-op member is a Link
// Play visitor; a request is a record of 0x3b0 payload bytes with its type u16 at +0x3b8 and its sequence number u16 at +0x3ba. The
// queue is u16 q[0..4] at BM + 0x9218378: on a visitor q[0] = requests sent, q[1] = answers taken, q[4] = the strict kind in flight
// (0xffff none); on the host q[slot] counts the requests taken from that member. HELD-EVENTS-RE: one dropped answer leaves
// q[0] != q[1] for the session, and 0x313418 then holds every queued event with flag bit 22 (the #792 class).
//   entry 0x2148a0(q, msg, &sender) the receive-side sequence check; a mismatch is dropped silently, no answer (recomputed here)
//   entry 0x214974(q, rec)          the send; refused on a visitor while strict and q[0] != q[1] or q[4] < 0x23 (recomputed here)
//   entry 0x2159c0(q, rec, entries) the talk-end record's send gate (types 10 / 11), called by the talk-end walk 0x480b60 at 0x48190c
//   entry 0x2197a0(q, sender, rec)  the host's apply of a member's talk-end record (cases 0xa / 0xb of 0x216860, which has already
//                                   counted the request: q[sender] + 1 and the answer's sequence number into rec +0x3ba)
// The queue's own changes are polled once a frame and written when a value changes.
#include <cstring>

#include "core/pkp_journal_services_internal.h"

namespace nxrt::games::pkp::journal::services {

namespace {

constexpr uint64_t kRecType = 0x3b8, kRecSeq = 0x3ba, kRecPipelined = 0x274;   // record: u16 type, u16 sequence, u32 flags (bit 6 pipelined)
constexpr uint16_t kMaxType = 0x22;                                             // 0x214974 refuses a type above it
constexpr uint32_t kStrictTypes = 0xa0000c00;                                   // pipelined records of these types (10, 11, 29, 31) are strict too
constexpr uint16_t kTalkEndPlain = 10, kTalkEndItems = 11;                      // talk-end record types; 10 is never strict
constexpr uint16_t kStrictAnswers = 0x23;                                       // q[4] must reach this before a strict record goes
constexpr uint64_t kEntrySize = 0x50, kEntryFlags = 8, kEntryActor = 0xe, kEntryItem = 0x18, kEntryCount = 0x1a;   // talk-end entry
constexpr uint32_t kEntryMax = 4;
constexpr uint64_t kPlayerChara = layout::kManagerPlayer;   // PM + 0x30: the local player; 0x2159c0 / 0x214974 return without one

struct Poll {
    bool have = false;
    uint16_t q[5] = {};
};
Poll g_poll;

uint16_t rd16(uint64_t a) { return guest::ok(a, 2) ? guest::rd<uint16_t>(a) : 0; }

bool has_player() {
    const uint64_t pm = guest::deref(base() + layout::kPlayerManager);
    return pm && guest::deref(pm + kPlayerChara);
}

// The pipelined window of 0x214974 (0x214d40..0x214d78): sent while the 16-bit counters, compared with wrap-around, leave q[0] at most
// 0x10 ahead of q[1].
bool window_open(uint16_t q0, uint16_t q1) {
    const uint32_t a = q0, b = static_cast<uint16_t>(q1 + 0x10);
    return ((a ^ b) < 0x8000) ? a <= b : (b & 0x7fff) <= (a & 0x7fff);
}

// Why 0x214974 / 0x2159c0 would not send this record (null: it would).
const char* strict_refusal(uint64_t q, uint16_t type, bool pipelined, char* buf, size_t n) {
    uint16_t v[5] = {};
    if (!q || !guest::read(q, v, sizeof v)) return "queue unreadable";
    const bool strict = type != kTalkEndPlain && (!pipelined || (type <= 0x1f && (kStrictTypes >> type & 1)));
    if (strict) {
        if (v[0] != v[1]) {
            std::snprintf(buf, n, "strict kind %u while q[0]=%u != q[1]=%u (a request is unanswered)", type, v[0], v[1]);
            return buf;
        }
        if (v[4] < kStrictAnswers) {
            std::snprintf(buf, n, "strict kind %u while q[4]=%u < 0x23", type, v[4]);
            return buf;
        }
    } else if (type != kTalkEndPlain && !window_open(v[0], v[1])) {
        std::snprintf(buf, n, "pipelined kind %u with q[0]=%u more than 0x10 ahead of q[1]=%u", type, v[0], v[1]);
        return buf;
    }
    return nullptr;
}

}  // namespace

void on_queue_receive(::Ctx* c) {
    const uint64_t q = c->x[0], msg = c->x[1], sender = c->x[2];
    if (!guest::ok(q, 10) || !guest::ok(msg + kRecType, 4)) return;
    const uint8_t sess = session();
    const uint16_t seq = rd16(msg + kRecSeq), type = rd16(msg + kRecType);
    const uint8_t slot = guest::ok(sender, 1) ? guest::rd<uint8_t>(sender) : 0xff;
    // 0x2148a0: GM missing -> drop; host (bit 1): q[slot - 1] + 1 for slot 1..4; visitor (bit 2): q[1] + 1; neither: drop.
    int expected = -1;
    const char* why = nullptr;
    char how[48] = "";
    if (!game_state()) why = "no game state";
    else if (is_host(sess)) {
        const uint8_t idx = static_cast<uint8_t>(slot - 1);
        if (idx > 3) {
            why = "host: sender slot outside 1..4";
        } else {
            expected = static_cast<uint16_t>(rd16(q + idx * 2) + 1);
            std::snprintf(how, sizeof how, "host: q[%u]+1", idx);
        }
    } else if (is_visitor(sess)) {
        expected = static_cast<uint16_t>(rd16(q + 2) + 1);
        std::snprintf(how, sizeof how, "visitor: q[1]+1");
    } else {
        why = "neither host nor visitor";
    }
    journal::Rec r(journal::Area::Sync, "linkplay-receive");
    r.in("slot=%u seq=%u m+0x3b8=%u q=%s sess=%s from=%s", slot, seq, type, queue_text(q).c_str(), session_text(sess).c_str(), lr_text(c).c_str());
    if (expected >= 0) r.expect("seq %d (%s)", expected, how);
    if (why) r.refused("dropped silently: %s", why);
    else if (seq != expected) r.refused("dropped silently: seq %u, expected %d", seq, expected);
    else r.out("taken");
}

void on_queue_send(::Ctx* c) {
    const uint64_t q = c->x[0], rec = c->x[1];
    if (!guest::ok(rec + kRecType, 4)) return;
    const uint8_t sess = session();
    const uint16_t type = rd16(rec + kRecType), seq = rd16(rec + kRecSeq);
    const bool pipelined = guest::ok(rec + kRecPipelined, 4) && (guest::rd<uint32_t>(rec + kRecPipelined) >> 6 & 1);
    journal::Rec r(journal::Area::Sync, "linkplay-send");
    r.in("type=%u seq=%u pipelined=%d q=%s sess=%s from=%s", type, seq, pipelined, queue_text(q).c_str(), session_text(sess).c_str(), lr_text(c).c_str());
    r.expect("sent");
    char buf[160];
    if (!is_visitor(sess)) r.refused("not a visitor: nothing is sent");
    else if (type > kMaxType) r.refused("type %u above 0x22", type);
    else if (!has_player()) r.refused("no local player");
    else if (const char* why = strict_refusal(q, type, pipelined, buf, sizeof buf)) r.refused("%s", why);
    else r.out("sent");
}

void on_record_send(::Ctx* c) {
    const uint64_t q = c->x[0], rec = c->x[1], entries = c->x[2];
    if (!guest::ok(rec + kRecType, 4)) return;
    const uint8_t sess = session();
    const uint16_t type = rd16(rec + kRecType);
    std::string list;
    for (uint32_t i = 0; i < kEntryMax && guest::ok(entries + (i + 1) * kEntrySize, 1); ++i) {
        const uint64_t e = entries + i * kEntrySize;
        const uint16_t actor = rd16(e + kEntryActor);
        if (actor & 0x800 || (actor >> 12) - 1u >= 4) continue;   // the game's own test for a used entry
        if (!list.empty()) list += ", ";
        list += journal::fmt("#%u{actor=0x%04x flags=0x%08x item=%u count=%u}", i, actor, guest::rd<uint32_t>(e + kEntryFlags), rd16(e + kEntryItem), rd16(e + kEntryCount));
    }
    journal::Rec r(journal::Area::Service, "talk-end-send");
    r.in("type=%u entries=[%s] q=%s sess=%s from=%s", type, list.c_str(), queue_text(q).c_str(), session_text(sess).c_str(), lr_text(c).c_str());
    r.expect("sent");
    // 0x2159c0: visitor, type 10 / 11, a local player; type 11 also needs q[0] == q[1] and q[4] >= 0x23.
    uint16_t v[5] = {};
    const bool have_q = q && guest::read(q, v, sizeof v);
    if (!is_visitor(sess)) r.refused("not a visitor");
    else if ((type & 0xfffe) != kTalkEndPlain) r.refused("type %u is not 10 / 11", type);
    else if (!has_player()) r.refused("no local player");
    else if (type == kTalkEndItems && (!have_q || v[0] != v[1] || v[4] < kStrictAnswers))
        r.refused("type 11 while q[0]=%u q[1]=%u q[4]=%u (needs q[0]==q[1] and q[4]>=0x23): queue busy", v[0], v[1], v[4]);
    else r.out("passed to the send 0x214974");
}

void on_host_apply(::Ctx* c) {
    const uint64_t q = c->x[0], rec = c->x[2];
    if (!guest::ok(rec + kRecType, 4)) return;
    journal::Rec r(journal::Area::Service, "talk-end-apply");
    r.in("sender=%d type=%u seq=%u q=%s sess=%s", static_cast<int8_t>(c->x[1]), rd16(rec + kRecType), rd16(rec + kRecSeq), queue_text(q).c_str(), session_text(session()).c_str());
    if (!is_host(session())) r.note("not a host here");
}

void queue_frame(::Ctx*, bool) {
    const uint8_t sess = session();
    const uint64_t q = sess ? request_queue() : 0;
    if (!q) {
        g_poll.have = false;   // a new session starts from a new baseline
        return;
    }
    uint16_t v[5];
    if (!guest::read(q, v, sizeof v)) return;
    if (g_poll.have && !std::memcmp(v, g_poll.q, sizeof v)) return;
    journal::Rec r(journal::Area::Sync, g_poll.have ? "linkplay-queue" : "linkplay-queue-start");
    r.in("sess=%s", session_text(sess).c_str());
    if (g_poll.have) r.out("q=[%u,%u,%u,%u,%u] was [%u,%u,%u,%u,%u]", v[0], v[1], v[2], v[3], v[4], g_poll.q[0], g_poll.q[1], g_poll.q[2], g_poll.q[3], g_poll.q[4]);
    else r.out("q=[%u,%u,%u,%u,%u]", v[0], v[1], v[2], v[3], v[4]);
    std::memcpy(g_poll.q, v, sizeof v);
    g_poll.have = true;
}

void queue_reset() { g_poll = Poll{}; }

}  // namespace nxrt::games::pkp::journal::services
