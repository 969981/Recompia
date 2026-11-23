// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's story records (pkp_coop_internal.h). The authority's story records reach every member's own save.
//
// Eight records of the save and one of the challenge manager (pkp_layout.h; modding-docs coop-v2/role-census/followup/STORY-RECORDS.md
// sections 1-2, QUEST-EFFECTS-RE C3, coop-v2/SHOP-CATALOG-RE.md) hold world state that the game writes only where co-op lets the
// story's world part run, the authority:
//   1 m_effectCommandSaveData (STGDAT, R+0x972ba8, 0x1e4 bytes): the quest effects 0x13 0x17 0x1b 0x1c 0x1d 0x21 0x2a 0x2e 0x31 0x32
//     0x33 (world kinds: on_quest_effect skips them on a member), the wild pop runtime (+0xb8 timer, +0xc1 popped: 0x1b44e0 / 0x1b8540
//     / 0x1ba440 return on a visitor), the Ver2 clear on a home set (0x13a608, returns on a visitor), the real timers' tick (+0x1c4,
//     0x113b98 from the time skip) and the contents actions of 0x12f690 that run in the authority's scheduler. The game copies only
//     +0x158 / +0x15a, once, at the join (0x572454).
//   2 m_charaTownFlag (CMNDAT, R+0x4a27cc, 0x400) and 3 m_targetStageForEnableMoveStage (R+0x4a32e0, 0x400): quest effects 0x06 0x10
//     0x26 0x2f 0x37 and story-line step action 6 (on_town_action leaves it to the authority).
//   4 m_storyStageUniqueFlag (R+0x4a26cc, 8 bytes per stage): effects 0x22 / 0x38 (world kinds), step actions 0 / 10.
//   5 the hint-group flags (R+0x7416bc, 0x10): effect 0x05 (a world kind); read by the wild spawn picker 0x1c2c60.
//   6 the story's materials drop (m_rocketHouse, CMNDAT, R+0x73b258, 0x20): its cell, electricity and stage (+0xc, which quest
//     condition 9 reads on each process). The drop's close steps it on the process whose screen confirmed (pkp_coop_deposit.cpp: a
//     member's deposit is stepped on the authority), and 0x7e70bc / 0x7e711c set the electricity from that process's own power.
//   7 the event campaign history (m_eventCampaignHistory, R+0x6cec38, 15 slots of 6 bytes): which campaigns this save has seen
//     (bit 0) and closed (bit 3). The campaign creatures' talk test 0x29d910 and the area test 0x284660 read it on each process;
//     the group's is the authority's (owner decision), so a member's test answers as the host's (role row 0x29dcc8).
//   8 the cleaning count (R+0x4a4401, u8): the sludge cleanings unique request type 0x2c tests (0x6142d0; Lapras's request 128 asks
//     for 80). Its writers 0x6520e0 / 0x652880 count only on the authority in a session (pkp_coop_requests.cpp on_clean_count), so
//     the group's count is the authority's.
//   9 the open areas (challenge manager CM + 0x17dd6 + area * 10, +8 u8, areas 1..31): which areas the shop rows and challenges may
//     come from. The area gate 0x630fb0 reads the byte on the host and answers "open" for every area on a visitor, so a member listed
//     the rows of areas the host never opened (hundreds of extra shop rows); a member now takes the host's branch there (role row
//     0x630fcc) and reads its own copy of the host's bytes. Only the area refresh 0x62f62c writes them: it clears the whole entry
//     array and its 0x62f7e0 refills the bytes from the current area and the visited bits, on the host only (0x62f7e0 returns at once
//     on a visitor), so a member's bytes are zero after each of its area loads, and the copy is written again as for any record. Every
//     area move of the group is the host's own (pkp_coop_travel.cpp), so the authority's refresh sees every area the group opens. Only
//     the open byte of each 10-byte entry is copied: its other fields index this process's own challenge lists, which only the
//     host-only path (0x62ffa0, 0x630240) builds, and none of the gate's callers reads them. The bytes hold the host's current area,
//     so the record is taken only in the authority's area.
// Nothing carries any of them, so a member's copy never was the group's: the host's town register, an NPC's specialty-work flag, the
// stage-2 start's clear of chara 0x5d's +0xc (0xb4b60 clears it only on a visitor), request 73's slot clear (0x13a9b0) stayed apart.
// Every field is copied: each is save data the game keeps per save, none a pointer or a handle of this process (the record's own
// first 8 bytes, the field object's vtable, are left out: pkp_layout.h starts it at +0x8). The host-only timers (+0xb8, +0x1c4) are
// ticked by no member, so the authority's value is the only one there is. Town bit 4 (a move pending) is acted on only by 0x115ee4,
// which skips a visitor: harmless as copied.
// The authority: every kSendEvery seconds (the story digest's period), under its own menus and talks too, it sends each record that
// changed since it last sent it (StoryRecord, a record per message), and all of them when this process or another member joins and every kAllEvery seconds (a
// copy sent while a member was not joined yet is sent again that way, as the requests' whole list is). The effect-command record is
// STGDAT: whether the game keeps one per area is not settled, so it carries the authority's area and a member takes it only there.
// A member keeps the authority's latest copy of each record (one received before it can be written waits, never dropped) and, once
// a frame while it is in the group's world, can walk and talks to no one (the same moment the shared parts take their changes),
// writes the copy into its save wherever its save differs. Each frame, not once per message: a member's stage load or its own writer
// changes the record with no message coming. Before a write that changes bytes it logs (at most once per kLogGap per record) how many
// bytes differed and how many of them this member wrote itself since the last write, so logs show whether members write them.
// The confirm check (a log line only; it changes nothing that is written, nor when): at a write that reverts bytes this member
// wrote itself, those bytes and the member's values are kept, one check per record at a time; each later copy of the authority (of
// the same area) that holds the member's value at one of them marks it made there too. After kCheckFor seconds (a copy of every
// record comes at least every kAllEvery), or once all are marked, one line says how many were found at the same offsets and the
// first offsets that were not. Found means the authority made the same write there; not found means it did not, or made it into
// another slot of a slot array (wild pops +0xb4, Ver2 slots +0x3c, real timers +0x1c4), or wrote the field again since.
// A member's own writers (STORY-RECORDS.md section 4 holes 3, 4, 5, 7), by what makes them run:
//   - the stage-2 start's clear of chara 0x5d (0xb4b60) runs on every process and differs only by role: the authority's value is
//     the group's, so the copy written here is right.
//   - request 73's slot clear (0x13a9b0): a member's completion makes the authority run it once (pkp_coop_outcomes.cpp replay).
//   - a member's own story-line steps (actions 0 / 10 into the stage flags): the authority runs the shared line's step
//     (pkp_coop_events.cpp apply_lines) and the bits come back with the next record.
//   - the contents actions (0x866 0xdf2 0xdf6 0xe11 0xe6d 0xe70 0xe56 0xe57 0xc1e 0x122f 0x92e 0x933 0x960 0x11a8 0x119e 0xe02 0x8fe
//     0x836 0x9c4 0x9ce 0xe51 0x1194 0xf..0x11; town bits 0xc85 0xe72 0xede 0xe60; hint groups 0xbd6 0x12c600) run from 0x12f690 for
//     whichever content record of this process's scheduler GM+0xd8 became due, so whose they are is the record's trigger, not the
//     action: no action tests the role. Records on a polled list (0..2) mostly run on the authority and on every member at the same
//     moment (alpha 6 logs: most content / record pairs leave both lists within the clock offset, the wild pop of content 258
//     included), so most of a member's writes are the authority's too; but some polled records ran on one process only (on a member,
//     or on the authority alone), whether from that process's own save state or from a condition on its local player (0x12483c has
//     player-distance and held-item cases) is not settled. A member also replays old polled records the authority ran long ago (at
//     a join or a stage entry): there the authority's copy is the right one, and forwarding a member's writes would undo the
//     group's newer state. A record on a notice list runs on the
//     process whose code posted that notice: of the scheduler's notifiers (0x1355ec..0x13664c, the callers of 0x1353a0) five skip a
//     visitor (0x1355ec talked to and 0x136548 fed, both posted on the authority for a member by pkp_coop_feed.cpp; 0x135ed8,
//     0x1363dc, 0x13664c) and the rest post on the acting player's own process (0x1356b8 / 0x135720 an event's end, list SC+0x8d0e0,
//     from the talker's runner 0x47e140; 0x135d80 a gimmick press, list SC+0xb6530, from the presser's 0x765b54; ...). A record on
//     such a list that writes these records is the case a member's write never reaches the authority. Which content records wait on
//     which notice and run which action is in the contents data (not parsed), so nothing is forwarded or skipped here; the check
//     below logs, for every own write the copy reverts, whether the authority's next copies carry it.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::records {

namespace {

constexpr const char* kTag = "pkp";

enum class Base : uint8_t { Save, Challenges };   // the save root (layout::kSaveRoot), the challenge manager (layout::kChallengeManager)

struct Record {
    uint8_t id;          // the wire's record id
    const char* name;
    Base base;
    uint64_t at;         // base + : the record's first byte
    size_t size;         // bytes copied
    bool per_area;       // taken only in the authority's area
    uint64_t stride = 1; // byte i is at base + at + i * stride
};
constexpr uint64_t kOpenAreasAt =
    layout::kChallengeAreas + layout::kChallengeAreaSize + layout::kChallengeAreaOpen;   // area 1's open byte
constexpr Record kRecords[] = {
    {1, "effect commands", Base::Save, layout::kSaveEffectCommands, layout::kEffectCommandsBytes, true},
    {2, "town flags", Base::Save, layout::kSaveTownFlags, layout::kTownRecordBytes, false},
    {3, "town targets", Base::Save, layout::kSaveTownTargets, layout::kTownRecordBytes, false},
    {4, "stage flags", Base::Save, layout::kSaveStageFlags, layout::kStageFlagsBytes, false},
    {5, "hint groups", Base::Save, layout::kSaveHintGroups, layout::kHintGroupsBytes, false},
    {6, "materials drop", Base::Save, layout::kSaveDrop, layout::kDropRecordBytes, false},
    {7, "campaign history", Base::Save, layout::kSaveCampaignHistory, layout::kCampaignHistoryBytes, false},
    {8, "cleaning count", Base::Save, layout::kSaveCleanCount, 1, false},
    {9, "open areas", Base::Challenges, kOpenAreasAt, layout::kChallengeAreaMax, true, layout::kChallengeAreaSize},
};
constexpr size_t kCount = sizeof kRecords / sizeof kRecords[0];
constexpr size_t kMaxBytes = layout::kTownRecordBytes;
static_assert(layout::kEffectCommandsBytes <= kMaxBytes && layout::kStageFlagsBytes <= kMaxBytes && layout::kHintGroupsBytes <= kMaxBytes &&
                  layout::kDropRecordBytes <= kMaxBytes && layout::kCampaignHistoryBytes <= kMaxBytes &&
                  layout::kChallengeAreaMax <= kMaxBytes,
              "every record fits a copy");
constexpr const Record& kOpenAreas = kRecords[8];
static_assert(kOpenAreas.id == 9 && kOpenAreas.base == Base::Challenges);

constexpr double kSendEvery = 5.0;   // the authority's look for changes: the story digest's period (pkp_coop_story.cpp)
constexpr double kAllEvery = 30.0;   // every record again
constexpr double kLogGap = 30.0;     // a member's "differed" line, at most once per this many seconds per record
constexpr double kCheckFor = kAllEvery + 2 * kSendEvery;   // a confirm check's wait: a full copy has come by then
constexpr size_t kCheckShown = 8;    // offsets named in a confirm line

struct Copy {
    bool have = false;
    uint8_t stage = 0;
    uint32_t seq = 0;    // a member: which received copy this is (the copy written last keeps its number)
    std::array<uint8_t, kMaxBytes> bytes{};
};

// A member: its own writes the last revert undid, waiting for the authority's later copies (the confirm check).
struct Check {
    bool active = false;
    uint8_t stage = 0;
    uint32_t seq = 0;                       // the copy that reverted them; only later copies count
    double since = 0;
    size_t total = 0, made = 0;             // own bytes reverted / found in a later copy of the authority
    std::array<uint8_t, kMaxBytes> value{}; // the member's value at each
    std::array<bool, kMaxBytes> open{};     // own and not found yet
};

std::array<double, kCount> never() {
    std::array<double, kCount> a;
    a.fill(-1e9);
    return a;
}

struct State {
    // The authority.
    Copy sent[kCount];                 // what was sent last, per record
    double sent_at = -1e9, all_at = -1e9;
    bool send_all = true;
    int settled_stage = -1;            // the area play last ran in (-1: none yet)
    // A member.
    Copy theirs[kCount];               // the authority's latest copy
    Copy written[kCount];              // what this save held right after the last write or check (the own-writes baseline)
    std::array<double, kCount> logged_at = never();
    unsigned unlogged[kCount] = {};    // writes since the last line
    Check checks[kCount];
};
State& st() {
    static State s;
    return s;
}

uint8_t area_stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

// The record's first byte here, or 0 when its save or manager is not there.
uint64_t record_at(const Record& rec) {
    const uint64_t b = guest::rd<uint64_t>(base() + (rec.base == Base::Save ? layout::kSaveRoot : layout::kChallengeManager));
    return b && guest::ok(b + rec.at, (rec.size - 1) * rec.stride + 1) ? b + rec.at : 0;
}
// The record's bytes at `at` (record_at), into out / from in.
void read_record(const Record& rec, uint64_t at, uint8_t* out) {
    if (rec.stride == 1) std::memcpy(out, reinterpret_cast<const void*>(at), rec.size);
    else
        for (size_t i = 0; i < rec.size; ++i) out[i] = guest::rd<uint8_t>(at + i * rec.stride);
}
void write_record(const Record& rec, uint64_t at, const uint8_t* in) {
    if (rec.stride == 1) std::memcpy(reinterpret_cast<void*>(at), in, rec.size);
    else
        for (size_t i = 0; i < rec.size; ++i) guest::wr<uint8_t>(at + i * rec.stride, in[i]);
}

size_t differing(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) d += a[i] != b[i];
    return d;
}

// A member: the confirm check's line, and the check ended.
void check_report(size_t i, const char* why) {
    Check& k = st().checks[i];
    std::string missing;
    size_t shown = 0;
    for (size_t p = 0; p < kRecords[i].size && shown < kCheckShown; ++p) {
        if (!k.open[p]) continue;
        char one[16];
        std::snprintf(one, sizeof one, "%s+0x%zx", shown ? " " : "", p);
        missing += one;
        ++shown;
    }
    if (k.made == k.total)
        NXLOG_I(kTag, "Co-op records: %s: all %zu bytes this member wrote itself were found at the same offsets in the authority's "
                "later copies (%s)", kRecords[i].name, k.total, why);
    else
        NXLOG_I(kTag, "Co-op records: %s: of %zu bytes this member wrote itself, %zu were found at the same offsets in the authority's "
                "later copies and %zu were not (%s; at %s%s)", kRecords[i].name, k.total, k.made, k.total - k.made, why,
                missing.c_str(), k.total - k.made > shown ? " ..." : "");
    k.active = false;
}

// A member: a later copy of the authority marks the own bytes it holds too.
void check_copy(size_t i, const Copy& t) {
    Check& k = st().checks[i];
    if (!k.active || t.seq <= k.seq || (kRecords[i].per_area && t.stage != k.stage)) return;
    for (size_t p = 0; p < kRecords[i].size; ++p) {
        if (!k.open[p] || t.bytes[p] != k.value[p]) continue;
        k.open[p] = false;
        ++k.made;
    }
    if (k.made == k.total) check_report(i, "a later copy");
}

// The authority: the records that changed since they were last sent (all of them when due).
void send(bool in_game) {
    State& s = st();
    // Not gated on in_game: that is play mode's "playing", false under every game menu, choice box and talk box, so the authority,
    // whose player is in one most of the time, sent only in the gaps (alpha 6: copies 32..110 s apart, changes waiting up to 40 s).
    // Reading its own save is safe on the game thread at any time; only the effect-command record (STGDAT) is held after a stage
    // change until play has run in the new area once, so a copy is never taken from the area data while it loads.
    if (!link::is_authority() || !(session_bits() & layout::kSessionOnline) || !link::member_count() || !local_player()) return;
    const uint8_t stage = area_stage();
    if (in_game) s.settled_stage = stage;
    const double now = qpc::seconds();
    if (now - s.sent_at < kSendEvery) return;
    s.sent_at = now;
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    for (size_t i = 0; i < kCount; ++i) {
        const Record& rec = kRecords[i];
        const uint64_t at = record_at(rec);
        if (!at || (rec.per_area && s.settled_stage != stage)) continue;   // sent once settled: its stage then differs from the last
        std::array<uint8_t, kMaxBytes> bytes;
        read_record(rec, at, bytes.data());
        const uint8_t* now_bytes = bytes.data();
        Copy& last = s.sent[i];
        const bool changed = !last.have || (rec.per_area && last.stage != stage) || std::memcmp(last.bytes.data(), now_bytes, rec.size);
        if (!all && !changed) continue;
        link::to_members(wire::Type::StoryRecord, wire::Writer(link::self())
                                                      .u8(rec.id)
                                                      .u8(stage)
                                                      .u16(static_cast<uint16_t>(rec.size))
                                                      .bytes(now_bytes, rec.size)
                                                      .take());
        last.have = true;
        last.stage = stage;
        std::memcpy(last.bytes.data(), now_bytes, rec.size);
    }
    if (all) {
        s.send_all = false;
        s.all_at = now;
    }
}

// A member in the group's world, at a moment its own story parts take changes too.
bool member_safe(bool in_game) {
    return role() == CoopRole::Visit && !link::is_authority() && link::in_session() && (session_bits() & layout::kSessionVisitor) &&
           session::can_walk(in_game) && !talk::talking();
}

// A member: the authority's copies written wherever this save differs.
void take(bool in_game) {
    State& s = st();
    if (!member_safe(in_game)) return;
    const uint8_t stage = area_stage();
    for (size_t i = 0; i < kCount; ++i) {
        const Record& rec = kRecords[i];
        const Copy& t = s.theirs[i];
        const uint64_t at = record_at(rec);
        if (!t.have || !at || (rec.per_area && t.stage != stage)) continue;
        std::array<uint8_t, kMaxBytes> bytes;
        read_record(rec, at, bytes.data());
        const uint8_t* here = bytes.data();
        Copy& w = s.written[i];
        const bool baseline = w.have && (!rec.per_area || w.stage == stage);
        if (std::memcmp(here, t.bytes.data(), rec.size)) {
            const size_t differ = differing(here, t.bytes.data(), rec.size);
            const size_t own = baseline ? differing(here, w.bytes.data(), rec.size) : 0;
            const double now = qpc::seconds();
            ++s.unlogged[i];
            if (now - s.logged_at[i] >= kLogGap) {
                if (baseline)
                    NXLOG_I(kTag, "Co-op records: %s: %zu bytes differ from the authority's copy, %zu of them written here since the last "
                            "write (%u writes since the last line); the authority's copy is written", rec.name, differ, own, s.unlogged[i]);
                else
                    NXLOG_I(kTag, "Co-op records: %s: %zu bytes differ from the authority's copy at the first write of this session; "
                            "the authority's copy is written", rec.name, differ);
                s.logged_at[i] = now;
                s.unlogged[i] = 0;
            }
            Check& k = s.checks[i];
            if (own && !k.active) {
                k.total = k.made = 0;
                for (size_t p = 0; p < rec.size; ++p) {
                    k.open[p] = here[p] != w.bytes[p] && here[p] != t.bytes[p];
                    k.value[p] = here[p];
                    k.total += k.open[p];
                }
                k.active = k.total != 0;
                k.stage = t.stage;
                k.seq = t.seq;
                k.since = now;
            }
            write_record(rec, at, t.bytes.data());
        }
        if (!baseline || w.seq != t.seq) w = t;   // the save holds the authority's copy now
    }
}

}  // namespace

void on_record_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint8_t id = r.u8(), stage = r.u8();
    const uint16_t n = r.u16();
    if (!r.ok() || link::is_authority()) return;
    for (size_t i = 0; i < kCount; ++i) {
        if (kRecords[i].id != id) continue;
        if (n != kRecords[i].size) return;   // a record of another size (another build's layout): skipped
        Copy got;
        r.bytes(got.bytes.data(), n);
        if (!r.ok()) return;
        Copy& t = st().theirs[i];
        got.have = true;
        got.stage = stage;
        got.seq = t.seq + 1;
        t = got;
        check_copy(i, t);
        return;
    }
    // A record this build does not know: skipped.
}

void tick(bool in_game) {
    send(in_game);
    take(in_game);
    const double now = qpc::seconds();
    for (size_t i = 0; i < kCount; ++i)
        if (st().checks[i].active && now - st().checks[i].since >= kCheckFor) check_report(i, "after the wait");
}

void on_session_change() {
    State& s = st();
    for (size_t i = 0; i < kCount; ++i) {
        s.sent[i].have = false;
        s.theirs[i].have = false;
        s.written[i].have = false;
        s.unlogged[i] = 0;
        s.checks[i].active = false;
    }
    s.send_all = true;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::records

// The digests' reader (pkp_coop_digest.cpp): the open areas as the area gate's host branch reads them here, area -> byte (non-zero).
namespace nxrt::games::pkp::coop::digest {

bool open_areas(Entries& out) {
    const coop::records::Record& rec = coop::records::kOpenAreas;
    const uint64_t at = coop::records::record_at(rec);
    if (!at) return false;
    for (uint32_t i = 0; i < rec.size; ++i)
        if (const uint8_t b = guest::rd<uint8_t>(at + i * rec.stride)) out.push_back({i + 1, b});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
