// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's Life Coins (pkp_coop_internal.h). The coins are each player's own, as in the game's Link Play: every
// buy takes them from the buyer's own save (purchases: only the buyer pays). Coins come from the challenges (town goals) of the world,
// and a challenge the group finishes in the authority's world pays every member its coins, once.
//
// The game, 2.0.1 (modding-docs coop-v2/MONEY-RE.md):
//   save root + kSaveMoney             the coins, u32 (CMNDAT): every reader and writer takes this process's own, visitor or not.
//   save root + kSaveChallenges + id * 0xd8   the state of challenge id 1..0x4b0 (CMNDAT), the player's own: 3 done, 4 claimed. A
//                                      visitor's game reads and writes the mirror of the host's (CM + id * 0x12 + 0xc) instead, so on a
//                                      member the own record is ours: it is the member's ledger ("paid here"), saved with the coins.
//   kClaim 0x645c90(CM, id)            the PC's claim: state 4 (0x639440) and the coins (kPay) only when not a visitor (0x645d18), so a
//                                      member's claim pays nothing.
//   kPay 0x637480(CM, id)              the coins += kReward(CM, id) (capped at 999999), then the "claim N challenges" progress (0x6427a0,
//                                      forwarded to the host on a visitor). Its only visitor caller is the host's goal update
//                                      0x6355c0: when the host's state 4 arrives for a goal this member's mirror held at 3 and the
//                                      update marks this member a participant (mirror +0x11 bit 0, 0x636ddc). In co-op that is rare
//                                      (a member's progress mostly reaches the host as the host's own), so members stayed at 0.
//   kReward 0x645d40(CM, id)           the goal's coins (table row +0x28), with the goal's bonus on top (a visitor reads the mirror).
// Rules:
//   - the authority sends the ids of every challenge claimed in its own save (ChallengesClaimed) to every member: when the set grew,
//     when a member joins and every kAllEvery seconds;
//   - a member pays itself kReward for each such id its ledger lacks (own state != 4), then marks it, kPayPerTick at a safe moment
//     (in the group's world, can walk, talks to no one), with one line in the message log per batch. A goal claimed before the
//     member joined pays it too: the group earned it;
//   - the game's own visitor pay (kPay on a member) runs only for an id not in the ledger, and marks it: one goal pays once;
//   - Cloud Island (stage 0x11) keeps its own challenges: nothing is sent or paid there.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::money {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kReward = 0x645d40;    // (CM, id) -> u32 the goal's coins for this process
constexpr uint32_t kIdMax = layout::kChallengeMax;
constexpr double kAllEvery = 30.0;        // the authority's whole set again (a member that missed one)
constexpr unsigned kPayPerTick = 32;

struct State {
    // the authority
    std::bitset<kIdMax + 1> sent;      // claimed at the last send
    bool send_all = true;
    double all_at = 0.0;
    // a member
    std::bitset<kIdMax + 1> claimed;   // claimed in the authority's save, as it last said
    unsigned batch_goals = 0;          // paid in the current batch (its line goes when it is done)
    uint64_t batch_coins = 0;
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

uint8_t stage() {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    return game && guest::ok(game + layout::kStateStage, 1) ? guest::rd<uint8_t>(game + layout::kStateStage) : 0;
}

// This process's own challenge records (0: no save yet).
uint64_t records() {
    const uint64_t r = save_root();
    return r && guest::ok(r + layout::kSaveChallenges, (kIdMax + 1) * layout::kChallengeSize) ? r + layout::kSaveChallenges : 0;
}
bool claimed_here(uint64_t recs, uint32_t id) {
    return guest::rd<uint8_t>(recs + id * layout::kChallengeSize) == layout::kChallengeClaimed;
}

bool in_play() {
    return (session_bits() & layout::kSessionOnline) && link::in_session() && stage() != layout::kStageCloudIsland;
}
// A member in the group's world (the visitor bit: the game's challenge code reads the host's mirror).
bool member_here() { return in_play() && !link::is_authority() && (session_bits() & layout::kSessionVisitor); }

// The authority: the ids claimed in its save, to every member when the set grew (or all is due).
void send_claimed(uint64_t recs) {
    State& s = st();
    std::bitset<kIdMax + 1> now;
    for (uint32_t id = 1; id <= kIdMax; ++id)
        if (claimed_here(recs, id)) now.set(id);
    const double t = qpc::seconds();
    if (!s.send_all && t - s.all_at < kAllEvery && (now & ~s.sent).none()) return;
    if (!link::member_count()) return;
    wire::Writer w(link::self());
    w.u16(static_cast<uint16_t>(now.count()));
    for (uint32_t id = 1; id <= kIdMax; ++id)
        if (now.test(id)) w.u16(static_cast<uint16_t>(id));
    link::to_members(wire::Type::ChallengesClaimed, w.take());
    s.sent = now;
    s.send_all = false;
    s.all_at = t;
}

void batch_line() {
    State& s = st();
    if (!s.batch_goals) return;
    char text[160];
    std::snprintf(text, sizeof text, "The group's challenges paid you %llu Life Coin%s (%u challenge%s)!",
                  static_cast<unsigned long long>(s.batch_coins), s.batch_coins == 1 ? "" : "s", s.batch_goals,
                  s.batch_goals == 1 ? "" : "s");
    notice::post(text);
    NXLOG_I(kTag, "Co-op coins: %u challenge%s claimed by the group paid %llu coins here", s.batch_goals,
            s.batch_goals == 1 ? "" : "s", static_cast<unsigned long long>(s.batch_coins));
    s.batch_goals = 0;
    s.batch_coins = 0;
}

// A member: pays the claimed goals its ledger lacks, kPayPerTick at most.
void pay(::Ctx* c, uint64_t recs) {
    State& s = st();
    const uint64_t cm = guest::rd<uint64_t>(base() + layout::kChallengeManager);
    const uint64_t coins = save_root() + layout::kSaveMoney;
    if (!cm || !guest::ok(coins, 4)) return;
    unsigned paid = 0;
    for (uint32_t id = 1; id <= kIdMax && paid < kPayPerTick; ++id) {
        if (!s.claimed.test(id)) continue;
        s.claimed.reset(id);
        if (claimed_here(recs, id)) continue;
        const uint32_t reward = static_cast<uint32_t>(guest::call(c, base() + kReward, cm, id));
        const uint64_t have = guest::rd<uint32_t>(coins);
        guest::wr<uint32_t>(coins, static_cast<uint32_t>(std::min<uint64_t>(have + reward, layout::kMoneyMax)));
        guest::wr<uint8_t>(recs + id * layout::kChallengeSize, layout::kChallengeClaimed);
        ++paid;
        ++s.batch_goals;
        s.batch_coins += reward;
    }
    if (s.claimed.none()) batch_line();
}

}  // namespace

int on_pay(::Ctx* c) {
    if (!member_here()) return 0;
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint64_t recs = records();
    if (!recs || id < 1 || id > kIdMax) return 0;
    if (claimed_here(recs, id)) {
        NXLOG_I(kTag, "Co-op coins: the game's own visitor pay of challenge %u skipped: it paid here already", id);
        return 1;   // the function's result is not read (0x6355c0 sets the state next)
    }
    guest::wr<uint8_t>(recs + id * layout::kChallengeSize, layout::kChallengeClaimed);
    NXLOG_I(kTag, "Co-op coins: the game pays challenge %u here (its visitor pay)", id);
    return 0;
}

void on_claimed_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    if (!r.ok()) return;
    for (uint16_t k = 0; k < n; ++k) {
        const uint16_t id = r.u16();
        if (!r.ok()) return;
        if (id >= 1 && id <= kIdMax) s.claimed.set(id);
    }
}

void tick(::Ctx* c, bool in_game) {
    if (!in_play()) return;
    const uint64_t recs = records();
    if (!recs) return;
    if (link::is_authority()) {
        send_claimed(recs);
        return;
    }
    if (c && member_here() && session::can_walk(in_game) && !talk::talking()) pay(c, recs);
}

void on_session_change() {
    State& s = st();
    s.sent.reset();
    s.send_all = true;
    s.claimed.reset();
    s.batch_goals = 0;
    s.batch_coins = 0;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::money
