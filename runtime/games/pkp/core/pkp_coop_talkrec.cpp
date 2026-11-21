// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a creature's talk record flag a member's talk arms (pkp_coop_internal.h). The flag is the group's: it
// lives in the creature's talk record of the authority's world, and a member's talk that sets it sets it there.
//
// The game, 2.0.1:
//   talk record    0x38 bytes per chara (< 0x41a), save root + layout::kTalkRec* (own / a visitor's copy of the host's / Cloud
//                  Island's): the save's m_TalkComponentSaveParam block (+8 float, the walk-up timer 0x3afafc writes), u8 +0x10 the
//                  flag here, m_fStorySkitTimer +0x14, ... (serializer 0x5eb6c0). Every reader and writer picks the record as 0x376c4 /
//                  0x77ecb0 say: Cloud Island's there, else the own records or, on a visitor, its copy.
//   +0x10 writers  script callback slot 195 0x4ae68c sets it (1) for the talk's creature and starts the player's own talk cooldown
//                  (save root + 0x7415b4, a slot of m_talkCoolTimeV2, no visitor select); the host's apply of a member's talk-end entry
//                  0x2197a0 sets it with 0x3ad29c(comp, 1) when the entry's flag word has bit 31 (0x21ca9c), which the talk-end walk
//                  0x480b60 takes from the talk delta's +0x248 bit 40 (0x4817dc); 0x4bd420 (from 0x4bce60, a reset of many records,
//                  and 0x1fa820's 0x4bd65c) and 0x4edf60 (from the event manager *(main+0x3565138)'s 0x4edd00, which returns at
//                  once on a visitor, and from 0x4ee360) clear it. Each marks the actor dirty
//                  (0x282dc4(.., 0x10), which the talk component's replication 0x2ac320 / 0x2ad820 carries) only on Cloud Island, and
//                  callback 195 sets the delta bit 40 only there (0x4ae804). So outside Cloud Island a member's set stays in its copy:
//                  the host's record never gets it.
//   +0x10 readers  conditions 29 / 31 of the talk-script check table main+0x34c3060 (0x3b09c4 / 0x3b0ba8, through its entries 29 / 31,
//                  0x4dd874 / 0x4ddbb8, and the talk event end step's 0x3affe0): for species 0x13 / 0xb8 with the event manager's
//                  byte bit 1 / bit 2 set, a set flag offers talk 0x1ba (kind 0x16) / 0x899 (kind 0x18) and restarts the talker's
//                  cooldown. A visitor outside Cloud Island gets 0 there whatever the record holds (0x3b0b84 / 0x3b0d68).
// So the flag is the creature's ("this creature's event talk is open"), kept in the host's world as the game keeps it on Cloud Island
// through bit 40. Here, a member in the session outside Cloud Island: callback 195's set (midhook 0x4ae7e8, reached only off Cloud
// Island, after the store into the member's copy) is sent to the authority (TalkRecArmed), which sets the flag in its own record, as
// its apply 0x3ad29c does there for the Cloud Island records. A member's clears are not sent: they are its own process's resets of its
// copy, and the authority's own resets clear the group's record. What the member's conditions 29 / 31 answer is left as it is.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <deque>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::talkrec {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kCharaReg = 23;             // at 0x4ae7e8: the talk component's chara (comp +0xc, 0x4ae700 / 0x4ae768 / 0x4ae788)
constexpr uint64_t kCompReg = 22;              // at 0x4ae7e8: the talk component, or the stand-in main+kNoComponent
constexpr uint64_t kNoComponent = 0x35691c0;   // main + : the stand-in talk component of an actor that has none (0x4ae780)
constexpr size_t kQueueMax = 32;

struct State {
    std::deque<uint16_t> asks;   // a member: charas whose flag was set here, not sent yet, in order
};
State& st() {
    static State s;
    return s;
}

}  // namespace

int on_armed(::Ctx* c) {
    if (!quests::member_rules()) return 0;
    const uint32_t chara = static_cast<uint32_t>(c->x[kCharaReg]);
    if (c->x[kCompReg] == base() + kNoComponent || chara >= layout::kFriendCount) return 0;
    State& s = st();
    if (s.asks.size() >= kQueueMax) s.asks.pop_front();
    s.asks.push_back(static_cast<uint16_t>(chara));
    NXLOG_I(kTag, "Co-op talk record: chara %u's flag +0x%x set here: sent to the authority", chara,
            static_cast<unsigned>(layout::kTalkRecArmed));
    return 0;
}

void on_armed_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    const uint32_t chara = r.u16();
    if (!r.ok() || !link::is_authority() || chara >= layout::kFriendCount) return;
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t at = root + layout::kTalkRecOwn + static_cast<uint64_t>(chara) * layout::kTalkRecSize + layout::kTalkRecArmed;
    if (!root || !guest::ok(at, 1)) return;
    if (guest::rd<uint8_t>(at)) return;
    guest::wr<uint8_t>(at, 1);
    NXLOG_I(kTag, "Co-op talk record: node %u set chara %u's flag +0x%x in the group's record", origin, chara,
            static_cast<unsigned>(layout::kTalkRecArmed));
}

void tick() {
    State& s = st();
    if (s.asks.empty()) return;
    if (!quests::member_rules()) {
        s.asks.clear();
        return;
    }
    while (!s.asks.empty()) {
        if (!link::to_authority(wire::Type::TalkRecArmed, wire::Writer(link::self()).u16(s.asks.front()).take())) break;   // next frame
        s.asks.pop_front();
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::talkrec
