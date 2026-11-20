// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's Life Coins (pkp_coop_internal.h). The coins are each player's own, as in the game's Link Play: every
// buy takes them from the buyer's own save (purchases: only the buyer pays). Coins come from the challenges (town goals) of the
// world, and the claims are per player too: each member claims each done challenge at the PC for itself, once, into its own coins.
//
// The game, 2.0.1 (modding-docs coop-v2/MONEY-RE.md):
//   save root + kSaveMoney             the coins, u32 (CMNDAT): every reader and writer takes this process's own, visitor or not.
//   save root + kSaveChallenges + id * 0xd8   the state of challenge id 1..0x4b0 (CMNDAT), the player's own: 3 done, 4 claimed. A
//                                      visitor's game reads and writes the mirror (CM + id * 0x12) instead: its three state writers
//                                      (0x63732c, the open 0x637040, the goal update 0x6355c0) take the own record only when not a
//                                      visitor. So on a member the own record is ours: it is the member's ledger ("paid here"),
//                                      saved with the coins.
//   mirror +0xc / +0xd                 a visitor's view of the challenge (+0xc: 3 to claim, 4 claimed; the PC's list 0x645f20 offers
//                                      a claim for +0xc == 3) and the host's own state, as the host's goal update 0x6355c0 last sent
//                                      it (@0x635964). The update sets +0xc from the host's state and from this player's slot (the
//                                      host's per-user claim bit): host claimed and this player not -> 4 (@0x6363f4, nothing paid);
//                                      this player is no participant of the goal in the host's save (the writer 0x633f60 leaves its
//                                      slot empty) -> 2 for a done goal (@0x636308). So a member who joined late, or whose host
//                                      claimed first, never had a claim button.
//   kClaim 0x645c90(CM, id)            the PC's claim: 0x639440 (on a visitor: the claim sent to the host, RequestChallengeProgress
//                                      +0x4b = 1), then the coins (kPay) only when `bl 0x376c4; tbnz w0` (0x645d14 / 0x645d18) says
//                                      not a visitor.
//   kPay 0x637480(CM, id)              the coins += kReward(CM, id) (capped at 999999), then the "claim N challenges" progress (0x6427a0,
//                                      forwarded to the host on a visitor). Its visitor callers: the claim (bent below) and the host's
//                                      goal update 0x6355c0 @0x636340, when its answer carries this player's claim bit and +0xc was 3.
//   kReward 0x645d40(CM, id)           the goal's coins (table row +0x28), with the goal's bonus on top (a visitor reads the mirror).
// Rules:
//   - a member's claim pays here at once (ClaimVisitor: the visitor answer at 0x645d18 reads 0); the claim still goes to the host,
//     which records it for this player as the game does;
//   - kPay on a member runs only for an id not in the ledger, and marks it: one challenge pays a member once, by whichever path;
//   - a member's view of every listed challenge the host has done (+0xd 3 or 4, or in the host's ChallengesClaimed set: a goal the
//     host claimed before this member joined may have had no goal update here) is its own: +0xc 4 when the ledger has it, else 3,
//     every frame (the PC reads it while open). The host's own claim no longer pays the members;
//   - the authority still sends the ids claimed in its save (ChallengesClaimed): members on older builds pay themselves from it (their
//     ledger keeps that to once). This build only reads it as "the host has done it", so a challenge an older build paid stays
//     claimed (its ledger has it), and one it did not is claimable here;
//   - Cloud Island (stage 0x11) keeps its own challenges: nothing is sent, paid or changed there.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <bitset>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::money {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint32_t kIdMax = layout::kChallengeMax;
constexpr double kAllEvery = 30.0;        // the authority's whole set again (a member that missed one)
constexpr uint64_t kFromClaim = 0x645d28;  // kPay's return address when the PC's claim calls it (0x645d24)
constexpr uint8_t kViewFrom = 2;          // mirror +0xc below this (0 none, 1 open or reopened by 0x6371b0) is the game's alone,
                                          // except 0 on a goal the host claimed (no goal update came here)

struct State {
    // the authority (for members on older builds)
    std::bitset<kIdMax + 1> sent;      // claimed at the last send
    bool send_all = true;
    double all_at = 0.0;
    // a member
    std::bitset<kIdMax + 1> host_claimed;   // claimed in the authority's save, as it last said
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

// A member: every challenge of the PC's list (CM + kChallengeList, the ids 0x645f20 offers) the host has done shows this member's own
// claim: claimed when the ledger has it, else to claim. +0xc below 2 (open, or a goal the update reopened, 0x6371b0) is left alone.
void own_view(uint64_t recs) {
    const uint64_t cm = guest::rd<uint64_t>(base() + layout::kChallengeManager);
    if (!cm || !guest::ok(cm, layout::kChallengeListCount + 2)) return;
    const int16_t n = guest::rd<int16_t>(cm + layout::kChallengeListCount);
    if (n < 1 || n > layout::kChallengeListMax) return;
    for (int16_t k = 0; k < n; ++k) {
        const uint16_t id = guest::rd<uint16_t>(cm + layout::kChallengeList + k * 2u);
        if (id < 1 || id > kIdMax) continue;
        const uint64_t m = cm + id * layout::kChallengeMirrorSize;
        const uint8_t host = guest::rd<uint8_t>(m + layout::kMirrorHostState);
        const uint8_t mine = guest::rd<uint8_t>(m + layout::kMirrorState);
        const bool claimed_there = st().host_claimed.test(id);
        if (!claimed_there && (host < layout::kChallengeDone || host > layout::kChallengeClaimed)) continue;
        if (mine > layout::kChallengeClaimed || (mine < kViewFrom && !(claimed_there && mine == 0))) continue;
        const uint8_t want = claimed_here(recs, id) ? layout::kChallengeClaimed : layout::kChallengeDone;
        if (mine != want) guest::wr<uint8_t>(m + layout::kMirrorState, want);
    }
}

}  // namespace

int on_pay(::Ctx* c) {
    if (!member_here()) return 0;
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint64_t recs = records();
    if (!recs || id < 1 || id > kIdMax) return 0;
    if (claimed_here(recs, id)) {
        NXLOG_I(kTag, "Co-op coins: challenge %u skipped: it paid here already", id);
        return 1;   // the function's result is not read (the claim returns 1 next, 0x6355c0 sets the state next)
    }
    guest::wr<uint8_t>(recs + id * layout::kChallengeSize, layout::kChallengeClaimed);
    NXLOG_I(kTag, "Co-op coins: challenge %u pays here (%s)", id,
            c->x[30] - base() == kFromClaim ? "this player's claim" : "the host's goal update");
    return 0;
}

int on_claim_visitor(::Ctx* c) {
    if (member_here()) c->x[0] = 0;   // the visitor answer: the claim's pay runs here, kPay's ledger keeps it to once
    return 0;
}

// Older builds pay from it; here it only says which challenges the host has done (a member claims its own).
void on_claimed_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    if (!r.ok()) return;
    std::bitset<kIdMax + 1> now;
    for (uint16_t k = 0; k < n; ++k) {
        const uint16_t id = r.u16();
        if (!r.ok()) return;
        if (id >= 1 && id <= kIdMax) now.set(id);
    }
    s.host_claimed = now;
}

void tick(::Ctx*, bool) {
    if (!in_play()) return;
    const uint64_t recs = records();
    if (!recs) return;
    if (link::is_authority()) {
        send_claimed(recs);
        return;
    }
    if (member_here()) own_view(recs);
}

void on_session_change() {
    State& s = st();
    s.sent.reset();
    s.send_all = true;
    s.host_claimed.reset();
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::money
