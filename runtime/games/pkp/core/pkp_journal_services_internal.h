// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: what the services watcher's files share (pkp_journal_services*.cpp): the guest reads every hook starts with (game state,
// save root, session byte), the one-line texts they print, the dedupe the frame-rate sites use, and each file's hooks and polls.
// RE: modding-docs coop-v2 VISITOR-SERVICES-RE, REQUESTS-RE, SHOP-STOCK-RE, DISCOVERY-RE, HELD-EVENTS-RE.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"
#include "nxrt/abi.h"

namespace nxrt::games::pkp::journal::services {

// ---- reads ----

// The game state GM (0 while it is not made).
inline uint64_t game_state() {
    const uint64_t gm = guest::deref(base() + layout::kGameState);
    return gm && guest::ok(gm, 0x100) ? gm : 0;
}
// GM+0x90, the session bits (0 offline or before the state exists). The raw byte is printed beside every named meaning.
inline uint8_t session() {
    const uint64_t gm = game_state();
    return gm ? guest::rd<uint8_t>(gm + layout::kStateSession) : 0;
}
inline bool is_visitor(uint8_t s) { return s & layout::kSessionVisitor; }
inline bool is_host(uint8_t s) { return s & layout::kSessionHost; }
// GM+0x18, the stage (area) this process is in; 0x11 is Cloud Island (0x77ecb0).
inline uint8_t stage() {
    const uint64_t gm = game_state();
    return gm ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}
constexpr uint8_t kCloudStage = 0x11;
// The save root R (0 while it is not mapped).
inline uint64_t save_root() {
    const uint64_t r = guest::deref(base() + layout::kSaveRoot);
    return r && guest::ok(r + layout::kSaveCommon, 0x1000) ? r : 0;
}
// The Link Play request queue u16 q[0..4] (BM + 0x9218378, BM = *(main + 0x34efc98)); 0 while there is no block map.
inline uint64_t request_queue() {
    const uint64_t bm = guest::deref(base() + layout::kWorld);
    return bm && guest::ok(bm + layout::kWorldRequests, 10) ? bm + layout::kWorldRequests : 0;
}

// ---- text ----

// "0x06(host visitor)": the raw session byte and its named bits.
inline std::string session_text(uint8_t s) {
    return journal::fmt("0x%02x(%s%s%s%s)", s, s & layout::kSessionOnline ? "online " : "", s & layout::kSessionHost ? "host " : "",
                        s & layout::kSessionVisitor ? "visitor " : "", s ? "" : "offline");
}
// The caller of a hooked function as main+0x..., or "ext 0x..." when it is not main's code (co-op's own call).
inline std::string lr_text(::Ctx* c) {
    const uint64_t off = c->x[30] - base();
    return off < 0x5000000 ? journal::fmt("main+0x%llx", static_cast<unsigned long long>(off)) : journal::fmt("ext 0x%llx", static_cast<unsigned long long>(c->x[30]));
}
// "[a,b,c,d,e]": the request queue's five counters.
inline std::string queue_text(uint64_t q) {
    uint16_t v[5] = {};
    if (q) guest::read(q, v, sizeof v);
    return journal::fmt("[%u,%u,%u,%u,%u]", v[0], v[1], v[2], v[3], v[4]);
}

// Who a 16-byte user id belongs to, by comparing it with this process's own id and the session's member table: "own", "m0".."m3",
// "null" (all zero) or "other#xxxx" (a short hash: ids are not written).
inline std::string user_text(uint64_t id_ptr) {
    uint8_t id[layout::kUserIdSize];
    if (!guest::read(id_ptr, id, sizeof id)) return "unreadable";
    bool zero = true;
    uint32_t h = 2166136261u;
    for (uint8_t b : id) {
        zero = zero && !b;
        h = (h ^ b) * 16777619u;
    }
    if (zero) return "null";
    const uint64_t r = save_root();
    uint8_t own[layout::kUserIdSize];
    if (r && guest::read(r + layout::kOwnUserId, own, sizeof own) && !std::memcmp(own, id, sizeof id)) return "own";
    for (uint32_t i = 0; r && i < layout::kMemberCount; ++i) {
        uint8_t m[layout::kUserIdSize];
        if (guest::read(r + layout::kMemberTable + i * layout::kMemberSize + layout::kMemberUserId, m, sizeof m) && !std::memcmp(m, id, sizeof id))
            return journal::fmt("m%u", i);
    }
    return journal::fmt("other#%04x", h & 0xffff);
}

// ---- dedupe for sites that run once per list build or per row ----
// hit(key) is true the first time a key is seen; later hits only count. frame() writes, at most every 30 s, one line naming the keys
// whose count grew since the last line.
class Seen {
public:
    bool hit(uint64_t key) {
        std::lock_guard<std::mutex> lk(m_);
        Item& it = items_[key];
        ++it.count;
        return it.count == 1;
    }
    // Keys with new repeats since the last call, as "key x count" texts (empty: nothing grew). `label` names a key.
    template <class Label>
    std::vector<std::string> grown(Label label) {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [k, it] : items_)
            if (it.count > it.reported && it.count > 1) {
                out.push_back(label(k) + journal::fmt(" x%u", it.count));
                it.reported = it.count;
            }
        return out;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        items_.clear();
    }

private:
    struct Item {
        uint32_t count = 0, reported = 1;
    };
    std::mutex m_;
    std::unordered_map<uint64_t, Item> items_;
};

// ---- hooks and polls, by file ----

// pkp_journal_services_queue.cpp: Link Play's request queue and the talk-end record's send and apply.
void on_queue_receive(::Ctx* c);   // entry 0x2148a0
void on_queue_send(::Ctx* c);      // entry 0x214974
void on_record_send(::Ctx* c);     // entry 0x2159c0
void on_host_apply(::Ctx* c);      // entry 0x2197a0
void queue_frame(::Ctx* c, bool in_game);
void queue_reset();

// pkp_journal_services_work.cpp: creature work and the talk-end forward.
void on_talk_end(::Ctx* c);        // entry 0x480b60
void on_service_offer(::Ctx* c);   // entry 0x48fdac
void on_service_start(::Ctx* c);   // entry 0x4984a0
void on_job_start(::Ctx* c);       // entry 0x34dc64

// pkp_journal_services_requests.cpp: general and unique requests.
void on_general_state(::Ctx* c);   // midhooks 0x629004 and 0x6290ac
void on_goal_unique(::Ctx* c);     // midhook 0x61858c
void on_goal_general(::Ctx* c);    // midhook 0x621be8
void on_friendship(::Ctx* c);      // entry 0x619410
void requests_frame(::Ctx* c, bool in_game);
void requests_reset();

// pkp_journal_services_shop.cpp: the shop's stock and its grants.
void on_stock_get(::Ctx* c);       // entry 0x646864
void on_stock_set(::Ctx* c);       // entry 0x646ae8
void on_shop_grant(::Ctx* c);      // entry 0xe889f8
void on_offline_row(::Ctx* c);     // entry 0xe87260
void shop_frame(::Ctx* c, bool in_game);
void shop_reset();

// pkp_journal_services_discovery.cpp: discovery setters, wild pops, the placement notice.
void on_dex_bit0(::Ctx* c);        // entries 0xa982b0, 0xa983d4, 0xa984e4, 0xa985c8: dex bits 0..3 of the creature in w0
void on_dex_bit1(::Ctx* c);
void on_dex_bit2(::Ctx* c);
void on_dex_bit3(::Ctx* c);
void on_habitat_found(::Ctx* c);   // entry 0xa96f5c
void on_habitat_scene(::Ctx* c);   // entry 0xa9802c
void on_recipe_learn(::Ctx* c);    // entry 0x2e7240
void on_move_learn(::Ctx* c);      // entry 0x3bcca8
void on_emote_learn(::Ctx* c);     // entry 0x2f5860
void on_building_set(::Ctx* c);    // entry 0x5c04d4
void on_wild_pop(::Ctx* c);        // entry 0x1240dc
void on_placed_notice(::Ctx* c);   // entry 0x13596c
void discovery_frame(::Ctx* c, bool in_game);
void discovery_reset();

}  // namespace nxrt::games::pkp::journal::services
