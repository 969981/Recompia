// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's scene holds (pkp_coop_internal.h): which characters another member's running event (a scene) holds.
// Link Play gives an event's characters to the process that plays it (0x476100 takes them); a scene queued on another process
// meanwhile is refused at its start (pkp_coop_contents.cpp waits for it). The holder's reason is not in the game's shared state, so
// every process shares its own: the talk-lock keys (pkp_coop_talk.cpp) of the characters its event runners' events list, while a
// runner is in states 1..5 (past its start, to its callback; a start that aborts goes 0 to 6 and is not sent). Sent on a change,
// again when a member joins; a leaver's set is dropped.
//   ER = [main+0x35650f8], slot * 0x2010: u32 +8 the state, u16 +0x10 the event, +0x1a88 the u16 actor ids, +0x1a90 their count
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nxrt::games::pkp::coop::scenes {

namespace {

constexpr uint64_t kRunners = 0x35650f8, kRunnerSize = 0x2010;
constexpr uint64_t kRunNow = 8, kRunEvent = 0x10, kRunActors = 0x1a88, kRunActorCount = 0x1a90;
constexpr uint32_t kSlots = 2;
constexpr uint32_t kFirstHeld = 1, kLastHeld = 5;   // wait .. callback; 0 is the start, 6.. end it
constexpr size_t kMaxKeys = 16;

struct State {
    uint64_t sign = 0;                                       // the runners' (event, busy) last looked at
    std::vector<uint32_t> mine;                              // the keys last sent
    std::unordered_map<uint8_t, std::vector<uint32_t>> others;   // node -> the keys its scenes hold
};
State& st() {
    static State s;
    return s;
}

void send(const std::vector<uint32_t>& keys) {
    wire::Writer w(link::self());
    w.u8(static_cast<uint8_t>(keys.size()));
    for (uint32_t k : keys) w.u32(k);
    link::share(wire::Type::SceneHolds, w.take());
}

}  // namespace

void tick(::Ctx* c) {
    State& s = st();
    if (!c || !link::in_session()) {
        s.sign = 0;
        s.mine.clear();
        return;
    }
    const uint64_t got = base() + kRunners;
    const uint64_t er = guest::ok(got, 8) ? guest::rd<uint64_t>(got) : 0;
    if (!er || !guest::ok(er + kSlots * kRunnerSize - 8, 8)) return;
    uint64_t sign = 0;
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        const uint64_t run = er + slot * kRunnerSize;
        const uint32_t now = guest::rd<uint32_t>(run + kRunNow);
        const bool busy = now >= kFirstHeld && now <= kLastHeld;
        sign = sign << 32 | (busy ? 0x10000u | guest::rd<uint16_t>(run + kRunEvent) : 0);
    }
    if (sign == s.sign) return;
    s.sign = sign;
    std::vector<uint32_t> keys;
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        const uint64_t run = er + slot * kRunnerSize;
        const uint32_t now = guest::rd<uint32_t>(run + kRunNow);
        if (now < kFirstHeld || now > kLastHeld) continue;
        const uint64_t list = guest::rd<uint64_t>(run + kRunActors), count = guest::rd<uint64_t>(run + kRunActorCount);
        if (count > 64 || (count && !guest::ok(list, count * 2))) continue;
        for (uint64_t i = 0; i < count && keys.size() < kMaxKeys; ++i) {
            const uint32_t kind = guest::rd<uint16_t>(list + i * 2) >> 12;
            if (kind < 2 || kind > 4) continue;   // players and empty entries
            const uint32_t key = talk::character_key(c, list + i * 2);
            if (key && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        }
    }
    if (keys == s.mine) return;
    s.mine = keys;
    send(keys);
}

void on_holds_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    std::vector<uint32_t> keys;
    for (uint8_t i = 0; i < n && r.ok(); ++i) keys.push_back(r.u32());
    if (!r.ok() || origin == link::self()) return;
    if (keys.empty()) s.others.erase(origin);
    else s.others[origin] = std::move(keys);
}

void on_member(int node, bool joined, bool self) {
    State& s = st();
    if (self) {
        s.others.clear();
        s.sign = 0;
        s.mine.clear();
        return;
    }
    if (!joined) s.others.erase(static_cast<uint8_t>(node));
    else if (!s.mine.empty()) send(s.mine);   // the newcomer learns what this process's scenes hold
}

bool held_elsewhere(uint32_t key) {
    if (!key) return false;
    for (const auto& [node, keys] : st().others)
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) return true;
    return false;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::scenes
