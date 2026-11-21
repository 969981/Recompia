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
//
// A gimmick's release at the end of a use takes the same queue on a member, and is lost the same way. The classes' close functions
// build a release record (type 0x1a; 0x19 / 0xf for some container kinds) and send it with the generic request send 0x214974(q, rec)
// only after `bl 0x313418` answered true, which on a visitor is 0x214910(q): q[0] == q[1]. A false answer skips the record and
// nothing tries again (the close is one-shot: its flags are cleared on entry, 0x7b69e8 / 0x83c2d0). 0x214974 itself also refuses a
// strict record (rec +0x274 bit 6 clear, as these memset records are) while q[0] != q[1] or q[4] < 0x23, silently. The host then
// keeps the member as the object's owner (its record handler 0x221d00 is the release: 0x538f60 take 0, quiet 0, then the container
// bookkeeping), so every other player's press shows "Someone else is using this right now." for the rest of the session. The gates
// (the `tbz w0, #0` after `bl 0x313418`; the false branch only skips the send and joins the true one after it):
//   0x7b8b68 in 0x7b8960, class 0x3268000's close (slot 22 0x7b7860): records 0x19 / 0x1a sent at 0x7b8dec, 0xf at 0x7b8c5c
//   0x82a558 in 0x82a460, class 0x32762f8's close: record 0x1a sent at 0x82a62c
//   0x7e20b0 in 0x7e2030 (class 0x326d7e0), 0x83bec0 in 0x83bc40 and 0x83c288 in 0x83c228 (class 0x3278b08), 0x83e310 in 0x83e1c0:
//     vt+0x490(g, force, 6, 1), whose queued path in 0x76792c builds record 0x1a and sends it at 0x767b84
// (0x85b430, the 0x327cf68 classes' release, sends record 0x1a at 0x85b7e0 with no gate; 0x214974 can still refuse it.) Here, on a
// member in a session, a false gate is turned true, and 0x214974 called from one of those sends (by its return address) keeps the
// record when the queue would refuse it or older records wait; the frame tick sends it with the game's own 0x214974 once the queue is
// idle. A host's release needs the sender to be the owner (0x538f60), so a late or repeated release of an object someone else took
// meanwhile changes nothing. Should the queue stay busy for kReleaseWait, the object is also given back at once with the forced
// SyncGimmickOwnership the classes use elsewhere (owner::give_back_cell); the record still goes once it can. A new take of the same
// object by this member drops a release still kept for it (that use ends with a release of its own).
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
constexpr uint64_t kSendRequest = 0x214974;  // (q, rec): the generic request send
constexpr uint64_t kFreeCounted = 0x1ad7290;  // (object): a counted object whose count reached 0
constexpr size_t kRecordSize = 0x3c0, kEntriesSize = 4 * 0x50;
constexpr uint64_t kRecordCounted = 0x3b0, kRecordType = 0x3b8, kCount = 0x20;
constexpr uint64_t kRecordCell = 0x258, kRecordKind = 0x2be;   // release records: packed cell u32 (x 12, y 8, z 12 bits), kind u16
constexpr uint16_t kTalkEndItems = 0xb;
constexpr uint64_t kSent = 0, kAnswered = 2, kStrictKind = 8;   // q offsets
constexpr uint16_t kNoKind = 0xffff, kStrictIdle = 0x23;          // q[4] >= kStrictIdle: no strict request in flight
constexpr size_t kHeldMax = 16;   // the queue's own window for requests ahead of their answers
constexpr int kTries = 3;
constexpr double kWaitLog = 10.0;
constexpr double kReleaseWait = 10.0;
// The return addresses of the release sends into 0x214974 (main offsets), see above.
constexpr uint64_t kReleaseSends[] = {0x7b8df0, 0x7b8c60, 0x82a630, 0x767b88, 0x85b7e4};

enum class Kind : uint8_t { TalkEnd, Release };

struct Held {
    Kind kind = Kind::TalkEnd;
    std::array<uint8_t, kRecordSize> rec;
    std::array<uint8_t, kEntriesSize> entries;
    int tries = 0;
    double since = 0;      // kept at
    bool given = false;    // a release: given back by Sync after kReleaseWait
};

struct State {
    std::deque<Held> held;
    bool sending = false;   // our own call into 0x2159c0 / 0x214974 is running
    double since = 0;       // the front record waits since
    bool logged_wait = false;
    uint32_t gates_opened = 0;   // gates turned true (logged the first few)
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

// The game's own test in 0x2159c0 (0x215a38..0x215a50) for a type 0xb record, and 0x214974's for a strict record.
bool idle(uint64_t q) {
    return guest::rd<uint16_t>(q + kSent) == guest::rd<uint16_t>(q + kAnswered) && guest::rd<uint16_t>(q + kStrictKind) >= kStrictIdle;
}

bool member_in_session() {
    return role() != CoopRole::Host && link::in_session() && !link::is_authority() && (session_bits() & layout::kSessionVisitor);
}

uint64_t counted(const Held& h) {
    uint64_t p;
    std::memcpy(&p, h.rec.data() + kRecordCounted, sizeof p);
    return guest::ok(p, kCount + 4) ? p : 0;
}

template <typename T>
T field(const Held& h, uint64_t off) {
    T v;
    std::memcpy(&v, h.rec.data() + off, sizeof v);
    return v;
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
    if (!s.held.empty()) NXLOG_W(kTag, "Co-op Link Play queue: %zu kept records dropped (%s)", s.held.size(), why);
    for (const Held& h : s.held) drop_ref(c, counted(h));
    s.held.clear();
    s.logged_wait = false;
}

const char* name(Kind k) { return k == Kind::Release ? "gimmick release" : "talk-end"; }

// A release record about to be dropped unsent: the object is given back by Sync first (unless that was done already), so no release
// is ever lost.
void give_back_unsent(::Ctx* c, Held& h, const char* why) {
    if (h.kind != Kind::Release || h.given) return;
    h.given = true;
    const uint32_t cell = field<uint32_t>(h, kRecordCell);
    const uint16_t kind = field<uint16_t>(h, kRecordKind);
    NXLOG_W(kTag, "Co-op Link Play queue: a gimmick release %s; kind %u at (%u,%u,%u) is given back by Sync", why, kind, cell & 0xfff,
            (cell >> 12) & 0xff, cell >> 20);
    owner::give_back_cell(c, kind, cell);
}

// Keeps the record (and, for a talk-end record, its entries) at the back of the queue.
void keep(::Ctx* c, Kind kind, uint64_t q, uint64_t rec, uint64_t entries) {
    State& s = st();
    if (s.held.size() >= kHeldMax) {
        NXLOG_W(kTag, "Co-op Link Play queue: more than %zu records waiting; the oldest (%s) is dropped", kHeldMax,
                name(s.held.front().kind));
        give_back_unsent(c, s.held.front(), "is dropped from a full queue");
        drop_ref(c, counted(s.held.front()));
        s.held.pop_front();
    }
    Held h;
    h.kind = kind;
    std::memcpy(h.rec.data(), reinterpret_cast<const void*>(rec), kRecordSize);
    if (entries) std::memcpy(h.entries.data(), reinterpret_cast<const void*>(entries), kEntriesSize);
    else h.entries.fill(0);
    hold_ref(counted(h));
    h.since = qpc::seconds();
    if (s.held.empty()) s.since = h.since;
    s.held.push_back(h);
    if (kind == Kind::Release) {
        const uint32_t cell = field<uint32_t>(h, kRecordCell);
        NXLOG_I(kTag, "Co-op Link Play queue: gimmick release kept (record type %u, kind %u, cell (%u,%u,%u); requests %u sent / %u "
                "answered, strict kind %u; %zu kept)", field<uint16_t>(h, kRecordType), field<uint16_t>(h, kRecordKind), cell & 0xfff,
                (cell >> 12) & 0xff, cell >> 20, guest::rd<uint16_t>(q + kSent), guest::rd<uint16_t>(q + kAnswered),
                guest::rd<uint16_t>(q + kStrictKind), s.held.size());
    } else {
        NXLOG_I(kTag, "Co-op Link Play queue: talk-end record with items kept (requests %u sent / %u answered, strict kind %u; %zu kept)",
                guest::rd<uint16_t>(q + kSent), guest::rd<uint16_t>(q + kAnswered), guest::rd<uint16_t>(q + kStrictKind),
                s.held.size());
    }
}

// A release kept for kReleaseWait while the queue stays busy: the object is given back by Sync now (once), so the others can use it
// while the record waits.
void give_back_waiting(::Ctx* c) {
    const double now = qpc::seconds();
    for (Held& h : st().held)
        if (now - h.since >= kReleaseWait) give_back_unsent(c, h, "waited for a busy queue");
}

}  // namespace

int on_send_record(::Ctx* c) {
    State& s = st();
    if (s.sending || !member_in_session()) return 0;
    const uint64_t q = c->x[0], rec = c->x[1], entries = c->x[2];
    if (!guest::ok(q, kStrictKind + 2) || !guest::ok(rec, kRecordSize) || !guest::ok(entries, kEntriesSize)) return 0;
    if (guest::rd<uint16_t>(rec + kRecordType) != kTalkEndItems || !local_player()) return 0;
    if (s.held.empty() && idle(q)) return 0;   // the game sends it now
    keep(c, Kind::TalkEnd, q, rec, entries);
    return 1;   // the game would have dropped it, or it waits behind an older one
}

int on_release_gate(::Ctx* c, uint64_t site) {
    if ((c->x[0] & 1) || !member_in_session()) return 0;
    c->x[0] = 1;   // the release record is built; 0x214974's hook below keeps it until the queue takes it
    State& s = st();
    if (s.gates_opened++ < 8)
        NXLOG_I(kTag, "Co-op Link Play queue: a gimmick release at main+0x%llx found a request in flight; it is kept instead of skipped",
                static_cast<unsigned long long>(site));
    return 0;
}

int on_send_request(::Ctx* c) {
    State& s = st();
    if (s.sending || !member_in_session()) return 0;
    const uint64_t from = c->x[30] - base();
    bool release = false;
    for (uint64_t r : kReleaseSends) release = release || from == r;
    if (!release) return 0;
    const uint64_t q = c->x[0], rec = c->x[1];
    if (!guest::ok(q, kStrictKind + 2) || !guest::ok(rec, kRecordSize)) return 0;
    if (s.held.empty() && idle(q)) return 0;   // the game sends it now
    keep(c, Kind::Release, q, rec, 0);
    c->x[0] = 0;   // every caller ignores the answer (each reads its record's counted object next)
    return 1;
}

void on_take(::Ctx* c, uint16_t kind, uint32_t cell) {
    State& s = st();
    const size_t before = s.held.size();
    for (auto it = s.held.begin(); it != s.held.end();) {
        if (it->kind == Kind::Release && field<uint16_t>(*it, kRecordKind) == kind && field<uint32_t>(*it, kRecordCell) == cell) {
            NXLOG_I(kTag, "Co-op Link Play queue: kind %u at (%u,%u,%u) was taken again; its kept release is dropped", kind,
                    cell & 0xfff, (cell >> 12) & 0xff, cell >> 20);
            drop_ref(c, counted(*it));
            it = s.held.erase(it);
        } else {
            ++it;
        }
    }
    if (s.held.size() != before) {
        s.since = qpc::seconds();
        s.logged_wait = false;
    }
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
    if (!q) return;
    if (!idle(q)) {
        give_back_waiting(c);
        if (!s.logged_wait && qpc::seconds() - s.since > kWaitLog) {
            s.logged_wait = true;
            NXLOG_W(kTag, "Co-op Link Play queue: %zu kept records wait %.0f s for the queue (requests %u sent / %u answered, strict "
                    "kind %u)", s.held.size(), qpc::seconds() - s.since, guest::rd<uint16_t>(q + kSent),
                    guest::rd<uint16_t>(q + kAnswered), guest::rd<uint16_t>(q + kStrictKind));
        }
        return;
    }
    Held& h = s.held.front();
    // A talk-end record waits until the player walks (its answer starts the host's job); a release goes at once, as the close sends it.
    if (h.kind == Kind::TalkEnd && !session::can_walk(in_game)) return;
    s.rec = h.rec;
    s.entries = h.entries;
    const uint16_t before = guest::rd<uint16_t>(q + kSent);
    s.sending = true;
    if (h.kind == Kind::Release)
        guest::call(c, base() + kSendRequest, q, reinterpret_cast<uint64_t>(s.rec.data()));
    else
        guest::call(c, base() + kSendRecord, q, reinterpret_cast<uint64_t>(s.rec.data()), reinterpret_cast<uint64_t>(s.entries.data()));
    s.sending = false;
    if (guest::rd<uint16_t>(q + kSent) == before && ++h.tries < kTries) return;
    if (guest::rd<uint16_t>(q + kSent) == before) {
        NXLOG_W(kTag, "Co-op Link Play queue: a kept %s record was not sent after %d tries; dropped", name(h.kind), kTries);
        give_back_unsent(c, h, "could not be sent");
    }
    else
        NXLOG_I(kTag, "Co-op Link Play queue: kept %s record sent after %.1f s (%zu left)", name(h.kind), qpc::seconds() - h.since,
                s.held.size() - 1);
    drop_ref(c, counted(h));
    s.held.pop_front();
    s.since = qpc::seconds();
    s.logged_wait = false;
}

bool unanswered() {
    const uint64_t q = queue();
    return q && guest::rd<uint16_t>(q + kSent) != guest::rd<uint16_t>(q + kAnswered);
}

bool kept() {
    return !st().held.empty();
}

void reset() {
    clear(nullptr, "co-op stopped");
    st().gates_opened = 0;
}

}  // namespace nxrt::games::pkp::coop::linkqueue
