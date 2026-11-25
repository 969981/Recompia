// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's bag records, what their files share (pkp_coop_bags.cpp has the whole account): the record and
// the module's state, and the few functions one file needs from another. For pkp_coop_bags*.cpp only; every other module uses the
// bags namespace of pkp_coop_internal.h.
#pragma once
#include "core/pkp_coop_internal.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::games::pkp::coop::bags {

inline constexpr size_t kSlots = layout::kBagWholeSlots;
static_assert(kSlots == wire::kBagRecordSlots && items::kSlotSize == wire::kBagSlotBytes, "the record is the game's whole bag");
inline constexpr uint64_t kBagBytes = kSlots * items::kSlotSize;

using Key = std::array<uint8_t, kMemberKeyBytes>;
using UserId = std::array<uint8_t, layout::kUserIdSize>;

struct Record {
    uint64_t seq = 0;   // 0: no record
    std::array<uint64_t, kSlots> slot{};   // each slot's payload (+8)
};

// A record with its file.
struct Kept {
    Record rec;
    bool dirty = false;
    double written_at = -1e9;
    UserId user{};   // the authority: the member's user id, from its last BagRecord (all 0: not known yet)
    int node = -1;   // the authority: the node that last sent this record in this run (-1: none)
};

// One credited gain: n more of the packed word `word` (items::kMoved without the count), found in bag slot `slot`.
struct Credit {
    uint64_t word = 0;
    uint32_t n = 0;
    size_t slot = 0;
};

// What the bag selector reads, as last seen.
struct Selection {
    uint64_t bag = 0;
    uint8_t stage = 0, own_bag = 0;
    bool visitor = false, online = false;
    bool player = false;   // the local player's character exists (it is made again at the end of a load)
    bool operator==(const Selection&) const = default;
};

struct State {
    bool loaded = false;      // own's file was read
    Kept own;                 // this process's player
    std::map<Key, Kept> members;   // the authority: every other member's, by key (read from its file on first use)
    bool active = false;      // in a session at the last tick
    bool answered = false;    // a member: the authority answered this session's first record (the authority: always)
    double hello_at = -1e9;
    bool send = false;        // a member: the record changed since the last BagRecord
    double sent_at = -1e9;
    bool apply = false;       // the record waits to be written into the selected bag
    double grace_until = 0.0;
    double refused_log_at = -1e9;
    Selection seen;
    bool looked = false;      // seen holds this session's first look
    double up_at = 0.0;       // the session came up
    bool was_ready = false;   // the bag was ready once in this session (or its absence was logged)
    uint32_t greeted = 0;     // the authority: bit per node whose first record of its session was answered
    double screen_at = -1e9;  // the bag / container screen's last frame (its hook runs on the game thread, as the tick)
    bool restore = false;     // a member: the host refused a request; the record waits to be written back into the bag
    double unanswered_at = -1.0;   // a member: when the request now unanswered was first seen so (-1: none)
    bool wait_logged = false; // that wait outlasted kAnswerWait and was logged
    double change_first = -1.0;   // a member: when the bag was first seen differing from the record (-1: it does not)
    double change_at = -1.0;      // and when it last changed since (the wait runs from there)
    Record change_bag;            // the bag as it was then
    // A source's gain (the header): the bag at gain_begin, and the credits not yet carried by a write-back.
    bool gain_open = false;
    uint64_t gain_bag = 0;
    Record gain_pre;
    uint32_t credited = 0;            // gains credited since the last write-back (its log line)
    std::vector<Credit> early;        // a member before the authority's answer: credits to put on top of the copy that wins
};

// ---- pkp_coop_bags.cpp ----
State& st();
Key own_key();
std::string hex(const Key& k);
// Slots in use and items in them.
std::pair<uint32_t, uint32_t> totals(const Record& r);
// The bag the game's selector picks now (0 while there is none), with what it read; whether it may be read and written now
// (*bad_slot: the first slot that is not made yet); its payloads as a record holds them.
Selection select(::Ctx* c);
bool ready(const Selection& sel, size_t* bad_slot);
void read_bag(uint64_t bag, Record* out);
// The bag / container screen drew a frame a moment ago.
bool screen_open();

// ---- pkp_coop_bags_store.cpp: the records' files ----
// This player's record (read from its file on first use) and, on the authority, another member's.
Kept& own();
Kept& member(const Key& k);
// Writes every changed record whose last write is old enough (all of them with force).
void flush(bool force);

// ---- pkp_coop_bags_credit.cpp: a gain credited at its source ----
// A payload's packed word (two stacks merge when theirs are equal), the game's max stack, and a credit put into a record by the
// rule's order (false when it fits nowhere).
uint64_t word_of(uint64_t p);
uint32_t max_stack(::Ctx* c);
bool place(Record& r, const Credit& g, uint32_t max);

}  // namespace nxrt::games::pkp::coop::bags
