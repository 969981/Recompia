// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's story records (pkp_coop_internal.h). The authority's story records reach every member's own save.
//
// Five records of the save (pkp_layout.h; modding-docs coop-v2/role-census/followup/STORY-RECORDS.md sections 1-2, QUEST-EFFECTS-RE C3)
// hold story state that the game writes only where co-op lets the story's world part run, the authority:
//   1 m_effectCommandSaveData (STGDAT, R+0x972ba8, 0x1e4 bytes): the quest effects 0x13 0x17 0x1b 0x1c 0x1d 0x21 0x2a 0x2e 0x31 0x32
//     0x33 (world kinds: on_quest_effect skips them on a member), the wild pop runtime (+0xb8 timer, +0xc1 popped: 0x1b44e0 / 0x1b8540
//     / 0x1ba440 return on a visitor), the Ver2 clear on a home set (0x13a608, returns on a visitor), the real timers' tick (+0x1c4,
//     0x113b98 from the time skip) and the contents actions of 0x12f690 that run in the authority's scheduler. The game copies only
//     +0x158 / +0x15a, once, at the join (0x572454).
//   2 m_charaTownFlag (CMNDAT, R+0x4a27cc, 0x400) and 3 m_targetStageForEnableMoveStage (R+0x4a32e0, 0x400): quest effects 0x06 0x10
//     0x26 0x2f 0x37 and story-line step action 6 (on_town_action leaves it to the authority).
//   4 m_storyStageUniqueFlag (R+0x4a26cc, 8 bytes per stage): effects 0x22 / 0x38 (world kinds), step actions 0 / 10.
//   5 the hint-group flags (R+0x7416bc, 0x10): effect 0x05 (a world kind); read by the wild spawn picker 0x1c2c60.
// Nothing carries any of them, so a member's copy never was the group's: the host's town register, an NPC's specialty-work flag, the
// stage-2 start's clear of chara 0x5d's +0xc (0xb4b60 clears it only on a visitor), request 73's slot clear (0x13a9b0) stayed apart.
// Every field is copied: each is save data the game keeps per save, none a pointer or a handle of this process (the record's own
// first 8 bytes, the field object's vtable, are left out: pkp_layout.h starts it at +0x8). The host-only timers (+0xb8, +0x1c4) are
// ticked by no member, so the authority's value is the only one there is. Town bit 4 (a move pending) is acted on only by 0x115ee4,
// which skips a visitor: harmless as copied.
// The authority: every kSendEvery seconds (the story digest's period) it sends each record that changed since it last sent it
// (StoryRecord, a record per message), and all of them when this process or another member joins and every kAllEvery seconds (a
// copy sent while a member was not joined yet is sent again that way, as the requests' whole list is). The effect-command record is
// STGDAT: whether the game keeps one per area is not settled, so it carries the authority's area and a member takes it only there.
// A member keeps the authority's latest copy of each record (one received before it can be written waits, never dropped) and, once
// a frame while it is in the group's world, can walk and talks to no one (the same moment the shared parts take their changes),
// writes the copy into its save wherever its save differs. Each frame, not once per message: a member's stage load or its own writer
// changes the record with no message coming. Before a write that changes bytes it logs (at most once per kLogGap per record) how many
// bytes differed and how many of them this member wrote itself since the last write, so logs show whether members write them.
// Known remaining hole (the member writes the authority never made; STORY-RECORDS.md section 4 holes 3, 4, 5, 7): the contents
// actions of the member's own scheduler (0x866 0xdf2 0xdf6 0xe11 0xe6d 0xe70 0xe56 0xe57 0xc1e 0x122f 0x92e 0x933 0x960 0x11a8 0x119e
// 0xe02 0x8fe 0x836 0x9c4 0x9ce 0xe51 0x1194 0xf..0x11; town bits 0xc85 0xe72 0xede 0xe60), script 0xbd6 (0x12c600, hint groups), a
// member's own completion of request 73 (0x13a9b0). When the authority's scheduler never ran the same entry, the write is overwritten
// here and lost for the group (a member's own story-line steps, actions 0 / 10 into the stage flags, are not: the authority runs the
// shared line's step, pkp_coop_events.cpp apply_lines, and the bits come back with the next record); forwarding them to the authority (as WildPop does for 0xc1e) needs a
// runtime answer to which entries a member's scheduler runs (QUEST-SCRIPT-RE section 5).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <array>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::records {

namespace {

constexpr const char* kTag = "pkp";

struct Record {
    uint8_t id;          // the wire's record id
    const char* name;
    uint64_t at;         // save root +
    size_t size;
    bool per_area;       // taken only in the authority's area
};
constexpr Record kRecords[] = {
    {1, "effect commands", layout::kSaveEffectCommands, layout::kEffectCommandsBytes, true},
    {2, "town flags", layout::kSaveTownFlags, layout::kTownRecordBytes, false},
    {3, "town targets", layout::kSaveTownTargets, layout::kTownRecordBytes, false},
    {4, "stage flags", layout::kSaveStageFlags, layout::kStageFlagsBytes, false},
    {5, "hint groups", layout::kSaveHintGroups, layout::kHintGroupsBytes, false},
};
constexpr size_t kCount = sizeof kRecords / sizeof kRecords[0];
constexpr size_t kMaxBytes = layout::kTownRecordBytes;
static_assert(layout::kEffectCommandsBytes <= kMaxBytes && layout::kStageFlagsBytes <= kMaxBytes && layout::kHintGroupsBytes <= kMaxBytes,
              "every record fits a copy");

constexpr double kSendEvery = 5.0;   // the authority's look for changes: the story digest's period (pkp_coop_story.cpp)
constexpr double kAllEvery = 30.0;   // every record again
constexpr double kLogGap = 30.0;     // a member's "differed" line, at most once per this many seconds per record

struct Copy {
    bool have = false;
    uint8_t stage = 0;
    uint32_t seq = 0;    // a member: which received copy this is (the copy written last keeps its number)
    std::array<uint8_t, kMaxBytes> bytes{};
};

struct State {
    // The authority.
    Copy sent[kCount];                 // what was sent last, per record
    double sent_at = -1e9, all_at = -1e9;
    bool send_all = true;
    // A member.
    Copy theirs[kCount];               // the authority's latest copy
    Copy written[kCount];              // what this save held right after the last write or check (the own-writes baseline)
    double logged_at[kCount] = {-1e9, -1e9, -1e9, -1e9, -1e9};
    unsigned unlogged[kCount] = {};    // writes since the last line
};
static_assert(kCount == 5, "logged_at has one start value per record");
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

uint8_t area_stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

// The record's bytes in this save, or 0 when the save is not there.
uint64_t record_at(uint64_t root, const Record& rec) {
    return root && guest::ok(root + rec.at, rec.size) ? root + rec.at : 0;
}

size_t differing(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) d += a[i] != b[i];
    return d;
}

// The authority: the records that changed since they were last sent (all of them when due).
void send(bool in_game) {
    State& s = st();
    if (!in_game || !link::is_authority() || !(session_bits() & layout::kSessionOnline) || !link::member_count()) return;
    const double now = qpc::seconds();
    if (now - s.sent_at < kSendEvery) return;
    s.sent_at = now;
    const uint64_t root = save_root();
    if (!root) return;
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    const uint8_t stage = area_stage();
    for (size_t i = 0; i < kCount; ++i) {
        const Record& rec = kRecords[i];
        const uint64_t at = record_at(root, rec);
        if (!at) continue;
        const uint8_t* now_bytes = reinterpret_cast<const uint8_t*>(at);
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
    const uint64_t root = save_root();
    const uint8_t stage = area_stage();
    for (size_t i = 0; i < kCount; ++i) {
        const Record& rec = kRecords[i];
        const Copy& t = s.theirs[i];
        const uint64_t at = record_at(root, rec);
        if (!t.have || !at || (rec.per_area && t.stage != stage)) continue;
        uint8_t* here = reinterpret_cast<uint8_t*>(at);
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
            std::memcpy(here, t.bytes.data(), rec.size);
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
        return;
    }
    // A record this build does not know: skipped.
}

void tick(bool in_game) {
    send(in_game);
    take(in_game);
}

void on_session_change() {
    State& s = st();
    for (size_t i = 0; i < kCount; ++i) {
        s.sent[i].have = false;
        s.theirs[i].have = false;
        s.written[i].have = false;
        s.unlogged[i] = 0;
    }
    s.send_all = true;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::records
