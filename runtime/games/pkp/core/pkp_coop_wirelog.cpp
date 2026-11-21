// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's messages in the journal (pkp_coop_internal.h wirelog, pkp_journal.h area Sync). The link calls it for
// every message it sends, receives, passes on or drops, so two players' logs show where a shared change stopped: sent and never
// received, received and dropped (why), received by the authority and passed on to which members, applied (the part's own line).
//   j.sync: send DexKnown to n0 | in: 9 bytes 03 01 01 00 19 00 03 | out: queued
//   j.sync: recv DexKnown from n2 origin n2 | in: 9 bytes ... | out: applied here, passed on to n1 n3
// A payload is shown as hex (its first kHexMax bytes), except MemberName, the purchase types and the save types, which carry a player's
// name, member keys or save file bytes: the log bundle hides names and keys in text, not in hex, so those show their size alone.
// The periodic types (StoryDigest, PurchaseAsk) are sent every few seconds with the same bytes: a payload equal to the last one of its
// type and peer is counted, not written, and the count goes on the next line that differs (at most kQuietFor seconds later).
// Also the journal's "who": this process's node and role in the session.
#include "core/pkp_coop_internal.h"
#include "core/pkp_journal.h"

#include <cstring>
#include <unordered_map>

#include "nxrt/coop_channel.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::wirelog {

namespace {

namespace ch = nxrt::coop_channel;
constexpr size_t kHexMax = 48;
constexpr double kQuietFor = 60.0;

bool private_payload(uint32_t type) {
    switch (static_cast<wire::Type>(type)) {
    case wire::Type::MemberName:
    case wire::Type::Purchase:
    case wire::Type::PurchaseLog:
    case wire::Type::SaveHave:    // member keys
    case wire::Type::SaveOffer:
    case wire::Type::SaveChunk:   // save file bytes
        return true;
    default:
        return false;
    }
}

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

struct Last {
    uint64_t hash = 0;
    double at = 0;
    uint32_t same = 0;   // equal payloads not written since
};
struct State {
    std::unordered_map<uint64_t, Last> last;   // key: direction (bit 63), type, peer
};
State& st() {
    static State s;
    return s;
}

// True when the line is to be written; *same gets the equal payloads skipped before it.
bool fresh(bool out, uint32_t type, int peer, const uint8_t* p, size_t n, uint32_t* same) {
    const uint64_t key = (out ? 1ull << 63 : 0) | static_cast<uint64_t>(type) << 16 | static_cast<uint16_t>(peer);
    Last& l = st().last[key];
    const uint64_t h = fnv(p, n);
    const double t = qpc::seconds();
    if (l.at != 0 && h == l.hash && t - l.at < kQuietFor) {
        ++l.same;
        return false;
    }
    *same = l.same;
    l = Last{h, t, 0};
    return true;
}

std::string payload_text(uint32_t type, const uint8_t* p, size_t n) {
    if (private_payload(type)) return journal::fmt("%zu bytes", n);
    return journal::fmt("%zu bytes ", n) + journal::hex_bytes(p, n, kHexMax);
}

journal::Who who() {
    journal::Who w;
    const int node = ch::self();
    if (!link::in_session() || node == ch::kNoNode) return w;
    w.node = node;
    w.role = link::is_authority() ? "host" : "member";
    return w;
}

}  // namespace

const char* type_name(uint32_t type) {
    switch (static_cast<wire::Type>(type)) {
    case wire::Type::StoryStates: return "StoryStates";
    case wire::Type::Ability: return "Ability";
    case wire::Type::TalkAsk: return "TalkAsk";
    case wire::Type::TalkDone: return "TalkDone";
    case wire::Type::TalkHolder: return "TalkHolder";
    case wire::Type::NamePrompt: return "NamePrompt";
    case wire::Type::Items: return "Items";
    case wire::Type::StoryDigest: return "StoryDigest";
    case wire::Type::MemberName: return "MemberName";
    case wire::Type::ReqUnique: return "ReqUnique";
    case wire::Type::ReqOutcome: return "ReqOutcome";
    case wire::Type::RecipeKnown: return "RecipeKnown";
    case wire::Type::WildPop: return "WildPop";
    case wire::Type::Purchase: return "Purchase";
    case wire::Type::PurchaseAsk: return "PurchaseAsk";
    case wire::Type::PurchaseLog: return "PurchaseLog";
    case wire::Type::DexKnown: return "DexKnown";
    case wire::Type::DexScene: return "DexScene";
    case wire::Type::HabitatKnown: return "HabitatKnown";
    case wire::Type::HabitatScene: return "HabitatScene";
    case wire::Type::EventHistory: return "EventHistory";
    case wire::Type::StoryLines: return "StoryLines";
    case wire::Type::QuestFlagAsk: return "QuestFlagAsk";
    case wire::Type::UnlockKnown: return "UnlockKnown";
    case wire::Type::AbilityCleared: return "AbilityCleared";
    case wire::Type::AreaMusic: return "AreaMusic";
    case wire::Type::ChallengesClaimed: return "ChallengesClaimed";
    case wire::Type::StateDigest: return "StateDigest";
    case wire::Type::StateAsk: return "StateAsk";
    case wire::Type::StateFull: return "StateFull";
    case wire::Type::StateReport: return "StateReport";
    case wire::Type::BagRecord: return "BagRecord";
    case wire::Type::BagRestore: return "BagRestore";
    case wire::Type::StoryRecord: return "StoryRecord";
    case wire::Type::FriendAsk: return "FriendAsk";
    case wire::Type::FriendState: return "FriendState";
    case wire::Type::CreatureEvent: return "CreatureEvent";
    case wire::Type::TravelAsk: return "TravelAsk";
    case wire::Type::NewcomerAsk: return "NewcomerAsk";
    case wire::Type::DepositAsk: return "DepositAsk";
    case wire::Type::DepositState: return "DepositState";
    case wire::Type::StoryFollow: return "StoryFollow";
    case wire::Type::SaveHave: return "SaveHave";
    case wire::Type::SaveOffer: return "SaveOffer";
    case wire::Type::SaveChunk: return "SaveChunk";
    case wire::Type::TalkRecArmed: return "TalkRecArmed";
    // a dropped player is waited for
    case wire::Type::DropHere: return "DropHere";
    case wire::Type::DropLeaving: return "DropLeaving";
    case wire::Type::DropWait: return "DropWait";
    case wire::Type::DropClosing: return "DropClosing";
    }
    return "?";
}

void install() { journal::set_who(&who); }

void reset() {
    st() = State{};
    journal::set_who(nullptr);
}

Pending sending(uint32_t type, int node, const uint8_t* p, size_t n) {
    Pending d;
    d.type = type;
    d.node = node;
    d.write = fresh(true, type, node, p, n, &d.same);
    if (d.write) d.payload = payload_text(type, p, n);
    return d;
}

void sent(const Pending& d, bool queued) {
    if (!d.write && queued) return;
    journal::Rec r(journal::Area::Sync, "send");
    r.note("%s to n%d", type_name(d.type), d.node);
    if (!d.payload.empty()) r.in("%s", d.payload.c_str());
    if (d.same) r.note("%u equal sends before", d.same);
    if (queued) r.out("queued");
    else r.refused("the channel did not take it (not in a session, or no such node)").warn();
}

void sent_all(uint32_t type, const uint8_t* p, size_t n, int count) {
    uint32_t same = 0;
    if (!fresh(true, type, -2, p, n, &same)) return;
    journal::Rec r(journal::Area::Sync, "send");
    r.note("%s to every member", type_name(type)).in("%s", payload_text(type, p, n).c_str()).out("queued for %d", count);
    if (same) r.note("%u equal sends before", same);
}

void received(uint32_t type, int from, uint8_t origin, const uint8_t* p, size_t n, const char* outcome) {
    uint32_t same = 0;
    if (!fresh(false, type, from, p, n, &same)) return;
    journal::Rec r(journal::Area::Sync, "recv");
    r.note("%s from n%d origin n%u", type_name(type), from, origin).in("%s", payload_text(type, p, n).c_str()).out("%s", outcome);
    if (same) r.note("%u equal before", same);
}

void forwarded(uint32_t type, uint8_t origin, const uint8_t* p, size_t n, const char* to) {
    uint32_t same = 0;
    if (!fresh(true, type, -3, p, n, &same)) return;
    journal::Rec r(journal::Area::Sync, "pass on");
    r.note("%s of origin n%u", type_name(type), origin).out("%s", *to ? to + 1 : "no member to pass it on to");
    if (same) r.note("%u equal before", same);
}

void dropped(uint32_t type, int from, const uint8_t* p, size_t n, const char* why) {
    journal::Rec r(journal::Area::Sync, "drop");
    r.note("%s from n%d", type_name(type), from).in("%s", payload_text(type, p, n).c_str()).refused("%s", why).warn();
}

}  // namespace nxrt::games::pkp::coop::wirelog
