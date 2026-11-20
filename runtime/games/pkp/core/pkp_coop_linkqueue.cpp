// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, Link Play's request queue on a member (pkp_coop_internal.h; RE: modding-docs coop-v2/VISITOR-SERVICES-RE.md
// sections 11 and 15). The queue is u16s at world +0x9218378 (layout::kWorldRequests): q[0] requests sent, q[1] answers taken, q[4] (+8)
// the strict kind in flight (0xffff when none). A strict send sets q[4] and the answer of that kind clears it (0x226ac0..0x226b18).
//   0x2159c0(q, rec, entries) (entry hook) sends the talk-end record (type u16 rec +0x3b8: 0xa, or 0xb when items are involved) that the
//   talk-end walk 0x480b60 (its one caller, 0x48190c) built: rec = 0x3b0 bytes copied into the message, +0x3b0 a counted object (count
//   u32 +0x20, freed by 0x1ad7290 at 0), +0x3ba / +0x3bc; entries = the up to four 0x50-byte actor entries (+0xe handle). Type 0xb goes
//   only while q[0] == q[1] and q[4] >= 0x23; otherwise the game returns before any write and the record is lost. The host then never
//   applies it: no job, no reward, no item transfer, and the creature stays owned by the member, so the host's job tick (0x3b39a4) and
//   everyone else's talks skip it from then on. Pickups and other requests are in flight often, so this happens in normal play.
//   Here a member in a session keeps such a record (and every later one, so they go in order) and sends it through the game's own
//   0x2159c0 from the frame tick once the queue is idle again and the player walks, one at a time (the send makes the queue busy until
//   its answer). The record is popped only when q[0] moved (the game sent it), so none goes twice; one the game would not send is
//   tried kTries times. Kept records are dropped when the session ends, at the next session start, and the oldest past kHeldMax.
//   0x214954(q) (entry hook), the visitor's queue reset at session start (only caller 0x534c74): it zeroes q[0..3] and leaves q[4]. q[4]
//   is otherwise reset only when the world is built (0x214890 from its constructor 0x2009a0, once per process). So a strict request
//   still out when a session ended left q[4] set in every later session of that process, and each talk-end record with items was
//   refused. The answer of the old session never comes; here q[4] is cleared with q[0..3], as the world's own reset does.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <array>
#include <atomic>
#include <deque>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::linkqueue {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kSendRecord = 0x2159c0;   // (q, rec, entries)
constexpr uint64_t kFreeCounted = 0x1ad7290;  // (object): a counted object whose count reached 0
constexpr size_t kRecordSize = 0x3c0, kEntriesSize = 4 * 0x50;
constexpr uint64_t kRecordCounted = 0x3b0, kRecordType = 0x3b8, kCount = 0x20;
constexpr uint16_t kTalkEndItems = 0xb;
constexpr uint64_t kSent = 0, kAnswered = 2, kStrictKind = 8;   // q offsets
constexpr uint16_t kNoKind = 0xffff, kStrictIdle = 0x23;          // q[4] >= kStrictIdle: no strict request in flight
constexpr size_t kHeldMax = 16;   // the queue's own window for requests ahead of their answers
constexpr int kTries = 3;
constexpr double kWaitLog = 10.0;

struct Held {
    std::array<uint8_t, kRecordSize> rec;
    std::array<uint8_t, kEntriesSize> entries;
    int tries = 0;
};

struct State {
    std::deque<Held> held;
    bool sending = false;   // our own call into 0x2159c0 is running
    double since = 0;       // the front record waits since
    bool logged_wait = false;
    alignas(16) std::array<uint8_t, kRecordSize> rec;   // the record and entries as the call reads them (guest addresses)
    alignas(16) std::array<uint8_t, kEntriesSize> entries;
};
State& st() {
    static State s;
    return s;
}

uint64_t queue() {
    const uint64_t world = guest::rd<uint64_t>(base() + layout::kWorld);
    return guest::ok(world, 8) && guest::ok(world + layout::kWorldRequests, kStrictKind + 2) ? world + layout::kWorldRequests : 0;
}

// The game's own test in 0x2159c0 (0x215a38..0x215a50) for a type 0xb record.
bool idle(uint64_t q) {
    return guest::rd<uint16_t>(q + kSent) == guest::rd<uint16_t>(q + kAnswered) && guest::rd<uint16_t>(q + kStrictKind) >= kStrictIdle;
}

bool member_in_session() {
    return role() != CoopRole::Host && link::in_session() && (session_bits() & layout::kSessionVisitor);
}

uint64_t counted(const Held& h) {
    uint64_t p;
    std::memcpy(&p, h.rec.data() + kRecordCounted, sizeof p);
    return guest::ok(p, kCount + 4) ? p : 0;
}

void hold_ref(uint64_t p) {
    if (p) std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p + kCount)).fetch_add(1, std::memory_order_acq_rel);
}

// Gives back the reference the kept record holds, freeing the object as the game does when it was the last. Without a guest context
// (reset) the last reference is left, so nothing is freed off the game thread.
void drop_ref(::Ctx* c, uint64_t p) {
    if (!p) return;
    std::atomic_ref<uint32_t> n(*reinterpret_cast<uint32_t*>(p + kCount));
    for (uint32_t v = n.load(std::memory_order_acquire); v;) {
        if (v == 1 && !c) return;
        if (n.compare_exchange_weak(v, v - 1, std::memory_order_acq_rel)) {
            if (v == 1) guest::call(c, base() + kFreeCounted, p);
            return;
        }
    }
}

void clear(::Ctx* c, const char* why) {
    State& s = st();
    if (!s.held.empty()) NXLOG_W(kTag, "Co-op Link Play queue: %zu kept talk-end records dropped (%s)", s.held.size(), why);
    for (const Held& h : s.held) drop_ref(c, counted(h));
    s.held.clear();
    s.logged_wait = false;
}

}  // namespace

int on_send_record(::Ctx* c) {
    State& s = st();
    if (s.sending || !member_in_session()) return 0;
    const uint64_t q = c->x[0], rec = c->x[1], entries = c->x[2];
    if (!guest::ok(q, kStrictKind + 2) || !guest::ok(rec, kRecordSize) || !guest::ok(entries, kEntriesSize)) return 0;
    if (guest::rd<uint16_t>(rec + kRecordType) != kTalkEndItems || !local_player()) return 0;
    if (s.held.empty() && idle(q)) return 0;   // the game sends it now
    if (s.held.size() >= kHeldMax) {
        NXLOG_W(kTag, "Co-op Link Play queue: more than %zu talk-end records waiting; the oldest is dropped", kHeldMax);
        drop_ref(c, counted(s.held.front()));
        s.held.pop_front();
    }
    Held h;
    std::memcpy(h.rec.data(), reinterpret_cast<const void*>(rec), kRecordSize);
    std::memcpy(h.entries.data(), reinterpret_cast<const void*>(entries), kEntriesSize);
    hold_ref(counted(h));
    if (s.held.empty()) s.since = qpc::seconds();
    s.held.push_back(h);
    NXLOG_I(kTag, "Co-op Link Play queue: talk-end record with items kept (requests %u sent / %u answered, strict kind %u; %zu kept)",
            guest::rd<uint16_t>(q + kSent), guest::rd<uint16_t>(q + kAnswered), guest::rd<uint16_t>(q + kStrictKind), s.held.size());
    return 1;   // the game would have dropped it, or it waits behind an older one
}

int on_session_reset(::Ctx* c) {
    if (!member_in_session()) return 0;
    clear(c, "a new Link Play session started");
    const uint64_t q = c->x[0];
    if (!guest::ok(q + kStrictKind, 2)) return 0;
    const uint16_t kind = guest::rd<uint16_t>(q + kStrictKind);
    if (kind == kNoKind) return 0;
    guest::wr<uint16_t>(q + kStrictKind, kNoKind);
    NXLOG_I(kTag, "Co-op Link Play queue: request kind %u was still out from an earlier session; cleared at session start", kind);
    return 0;   // the game's own reset zeroes q[0..3]
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (s.held.empty()) return;
    if (!member_in_session()) return clear(c, "the session ended");
    const uint64_t q = queue();
    if (!q || !session::can_walk(in_game)) return;
    if (!idle(q)) {
        if (!s.logged_wait && qpc::seconds() - s.since > kWaitLog) {
            s.logged_wait = true;
            NXLOG_W(kTag, "Co-op Link Play queue: %zu talk-end records wait %.0f s for the queue (requests %u sent / %u answered, strict "
                    "kind %u)", s.held.size(), qpc::seconds() - s.since, guest::rd<uint16_t>(q + kSent),
                    guest::rd<uint16_t>(q + kAnswered), guest::rd<uint16_t>(q + kStrictKind));
        }
        return;
    }
    Held& h = s.held.front();
    s.rec = h.rec;
    s.entries = h.entries;
    const uint16_t before = guest::rd<uint16_t>(q + kSent);
    s.sending = true;
    guest::call(c, base() + kSendRecord, q, reinterpret_cast<uint64_t>(s.rec.data()), reinterpret_cast<uint64_t>(s.entries.data()));
    s.sending = false;
    if (guest::rd<uint16_t>(q + kSent) == before && ++h.tries < kTries) return;
    if (guest::rd<uint16_t>(q + kSent) == before)
        NXLOG_W(kTag, "Co-op Link Play queue: a kept talk-end record was not sent after %d tries; dropped", kTries);
    else
        NXLOG_I(kTag, "Co-op Link Play queue: kept talk-end record sent after %.1f s (%zu left)", qpc::seconds() - s.since,
                s.held.size() - 1);
    drop_ref(c, counted(h));
    s.held.pop_front();
    s.since = qpc::seconds();
    s.logged_wait = false;
}

void reset() { clear(nullptr, "co-op stopped"); }

}  // namespace nxrt::games::pkp::coop::linkqueue
