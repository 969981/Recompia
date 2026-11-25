// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's party facts, the bags (pkp_coop_internal.h): how many of an item each player of the session
// holds, for the checks the game makes of one player's bag (the count rows of pkp_coop_party_table.inc).
//
// The game's count 0x2e9d74(player, &query, 0, 1) reads one of this process's own two bags (by the player's index byte +0x5a39
// and the role) and, with its w2 clear, sums the count byte of every slot whose item id (the slot word's low 13 bits) is the
// query's (0x2e9fb4..0x2e9ff8). No process holds another player's bag in a form that count reads. What co-op has is each member's
// bag record on the authority (pkp_coop_bags.cpp: the 130 slot payloads, the truth for that player's bag).
//   The authority: once a frame it looks at the records' sequence numbers (its own and each present member's); when one changed it
//   sums that record by item id, and at most every kSendEvery seconds it sends every present player's sums to every member
//   (PartyBags, the whole party in one message, so a member's table is replaced whole and a player who left is gone from it).
//   Sent again when a member joins or leaves.
//   A member: keeps the table it was sent, without its own row (its own count is the game's). Before the first one arrives it knows
//   no other bag, and a count row answers with its own bag alone.
// A player's count of an item here is the sum over the whole record, the same sum the game makes over that player's bag.
#include "core/pkp_coop_internal.h"
#include "core/pkp_items.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::party {

namespace {

constexpr const char* kTag = "pkp";
constexpr double kSendEvery = 2.0;
constexpr size_t kPlayers = 8;     // more rows than a session has players
constexpr size_t kKinds = 130;     // a bag's slots: at most this many item ids

struct Bag {
    uint8_t node = 0;
    uint64_t seq = 0;                                   // the authority: the record's sequence these sums were made from
    std::vector<std::pair<uint16_t, uint16_t>> items;   // item id, how many
};

struct State {
    std::vector<Bag> bags;   // the authority: every present player's, its own too; a member: the other players'
    uint32_t present = 0;    // the authority: a bit per member node in the session
    bool dirty = false;      // the authority: the table changed since the last send
    double sent_at = -1e9;
    bool logged = false;     // a member: the first table of the session was logged
};
State& st() {
    static State s;
    return s;
}

std::vector<std::pair<uint16_t, uint16_t>> sums(const uint64_t* slots, size_t n) {
    std::vector<std::pair<uint16_t, uint16_t>> out;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t id = items::id_of(slots[i]), count = items::count_of(slots[i]);
        if (!id || !count) continue;
        bool found = false;
        for (auto& [k, v] : out)
            if (k == id) {
                v = static_cast<uint16_t>(v + count);
                found = true;
                break;
            }
        if (!found) out.emplace_back(static_cast<uint16_t>(id), static_cast<uint16_t>(count));
    }
    return out;
}

uint32_t count_in(const Bag& b, uint32_t id) {
    for (const auto& [k, v] : b.items)
        if (k == id) return v;
    return 0;
}

void send(State& s) {
    wire::Writer w(link::self());
    w.u8(static_cast<uint8_t>(s.bags.size()));
    for (const Bag& b : s.bags) {
        w.u8(b.node).u8(static_cast<uint8_t>(b.items.size()));
        for (const auto& [id, count] : b.items) w.u16(id).u16(count);
    }
    link::to_members(wire::Type::PartyBags, w.take());
    s.dirty = false;
    s.sent_at = qpc::seconds();
}

// The authority: the table follows the records.
void follow_records(State& s) {
    bags::View views[kPlayers];
    const size_t n = bags::records(views, kPlayers);
    for (size_t i = 0; i < s.bags.size();) {   // a player with no record or no longer present
        bool keep = false;
        for (size_t v = 0; v < n; ++v)
            keep = keep || (views[v].node == s.bags[i].node && (views[v].node == link::self() || (s.present >> views[v].node) & 1));
        if (keep) {
            ++i;
            continue;
        }
        s.bags.erase(s.bags.begin() + static_cast<std::ptrdiff_t>(i));
        s.dirty = true;
    }
    for (size_t v = 0; v < n; ++v) {
        const bags::View& view = views[v];
        if (view.node != link::self() && !((s.present >> view.node) & 1)) continue;
        Bag* b = nullptr;
        for (Bag& have : s.bags)
            if (have.node == view.node) b = &have;
        if (b && b->seq == view.seq) continue;
        if (!b) {
            s.bags.push_back(Bag{static_cast<uint8_t>(view.node), 0, {}});
            b = &s.bags.back();
        }
        b->seq = view.seq;
        b->items = sums(view.slots, wire::kBagRecordSlots);
        s.dirty = true;
    }
}

}  // namespace

uint32_t teammates_most(uint32_t id, uint8_t* node) {
    uint32_t most = 0;
    for (const Bag& b : st().bags) {
        if (b.node == link::self()) continue;
        const uint32_t have = count_in(b, id);
        if (have > most) {
            most = have;
            *node = b.node;
        }
    }
    return most;
}

bool teammate_holds(uint32_t id, uint32_t exactly, uint8_t* node) {
    for (const Bag& b : st().bags) {
        if (b.node == link::self() || count_in(b, id) != exactly) continue;
        *node = b.node;
        return true;
    }
    return false;
}

void on_bags_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    if (link::is_authority()) return;
    State& s = st();
    std::vector<Bag> got;
    const uint8_t players = r.u8();
    for (uint8_t p = 0; p < players && r.ok() && p < kPlayers; ++p) {
        Bag b;
        b.node = r.u8();
        const uint8_t kinds = r.u8();
        for (uint8_t k = 0; k < kinds && r.ok() && k < kKinds; ++k) {
            const uint16_t id = r.u16(), count = r.u16();
            b.items.emplace_back(id, count);
        }
        if (b.node != link::self()) got.push_back(std::move(b));
    }
    if (!r.ok()) return;
    s.bags = std::move(got);
    if (!s.logged) {
        s.logged = true;
        NXLOG_I(kTag, "Co-op party: the bags of %zu other players are known here from now on", s.bags.size());
    }
}

void on_member_change(int node, bool joined) {
    State& s = st();
    if (!link::is_authority() || node < 0 || node >= 32) return;
    s.present = joined ? s.present | (1u << node) : s.present & ~(1u << node);
    s.dirty = true;   // a newcomer needs the table; a player who left is taken out of everyone's
}

void bags_tick() {
    State& s = st();
    if (!link::in_session()) {
        if (!s.bags.empty() || s.present || s.logged) s = State{};
        return;
    }
    if (!link::is_authority()) return;
    follow_records(s);
    if (s.dirty && link::member_count() && qpc::seconds() - s.sent_at >= kSendEvery) send(s);
}

void bags_reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::party
