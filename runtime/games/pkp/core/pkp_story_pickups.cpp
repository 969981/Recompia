// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: Story co-op's own copy of a world's important items for every visitor (pkp_story_pickups.h).
//
// The drop manager of the 2.0.1 main (*(game state + 0xb8)) has one 0xd0-byte entry per item lying in the world: +0 -> the item's
// element in the save's drop list (+0x10 the item word: bits 0-12 the id, 13-20 the variant; +0x20 16 bytes, its place; +0x30 flags,
// bit 1 set by the bag's drop, 0x2ebe0c), +0xc6 the list (2 = important), +0xc7 the player who claimed it (0xff: free).
// - 0x9cdba0(manager, entry, player, auto, forced): a pickup. A visitor's own try (forced 0) sends the request to the host; the
//   host's answer runs it again with forced 1 (0x22a544). The entry hook notes a forced pickup of the local player and ends a try
//   for an item already noted.
// - 0x222a20: the host's handler of a visitor's request. After giving the item it removes the entry: 0x222cf0, `bl 0x9cb680` with
//   x0 = the entry (the removal clears the save element and tells every player). The midhook there marks the entry and frees its
//   claim (0x9cbacc, the game's own), and the entry hook on 0x9cb680 ends that one call.
//   A memory piece takes the handler's other branch: claimed for the visitor (0x222b44), given (0x222b9c), left in the world, and
//   the claim is never freed, so the host, whose own pickup needs a free entry, cannot take that piece until its world is loaded
//   again. The midhook at 0x222ba0 (the branch's last instruction, x21 = the entry) frees it.
// - 0x798720(item id): the item is a memory piece.
#include "core/pkp_story_pickups.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_save_files.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/notices.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp {

namespace {

namespace stdfs = std::filesystem;
constexpr const char* kTag = "pkp";
constexpr const char* kFolder = "story-pickups";
constexpr double kNoticeSeconds = 5.0;

constexpr uint64_t kPickup = 0x9cdba0, kRemove = 0x9cb680, kHostRemoveSite = 0x222cf0, kHostPieceSite = 0x222ba0;
constexpr uint64_t kPieceEntryReg = 21;
constexpr uint64_t kIsMemoryPiece = 0x798720, kClaim = 0x9cbacc;
constexpr uint64_t kEntrySize = 0xd0, kEntryList = 0xc6, kEntryClaim = 0xc7;
constexpr uint8_t kListImportant = 2, kClaimFree = 0xff;
constexpr uint64_t kElementWord = 0x10, kElementPlace = 0x20, kElementFlags = 0x30, kElementSize = 0x40;
constexpr uint8_t kFlagBagDrop = 0x02;
constexpr uint32_t kItemIdMask = 0x1fff, kItemIdMax = 0x11f7, kItemKeyMask = 0x1fffff;   // id and variant

enum class SiteKind : uintptr_t { Pickup, Remove, HostRemove, HostPiece };
struct Site {
    uint64_t site;
    uint32_t word;
    SiteKind kind;
    bool entry;
};
constexpr Site kSites[] = {
    {kPickup, 0xb40045e1u, SiteKind::Pickup, true},              // cbz x1, ...
    {kRemove, 0xd503237fu, SiteKind::Remove, true},              // pacibsp
    {kHostRemoveSite, 0x941ea264u, SiteKind::HostRemove, false}, // bl 0x9cb680
    {kHostPieceSite, 0x17ffffcbu, SiteKind::HostPiece, false},   // b 0x222acc
};
constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t ids[kSiteCount] = {};         // all 0: off or not installed
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    stdfs::path file;
    // The game thread's (the drop manager runs there):
    std::unordered_set<std::string> taken;
    uint64_t keep = 0;                     // the host's entry whose removal, the next call of 0x9cb680, is ended
    uint8_t claim = 0;                     // 0x9cbacc's player byte
    double told_at = -1e9;
    bool logged_host = false, logged_visitor = false, logged_piece = false, write_failed = false;
};
State& st() {
    static State s;
    return s;
}

uint8_t session(const State& s, uint8_t* stage) {
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    if (!guest::ok(game, layout::kStateSession + 1)) return 0;
    if (stage) *stage = guest::rd<uint8_t>(game + layout::kStateStage);
    return guest::rd<uint8_t>(game + layout::kStateSession);
}

// The entry is an item this module gives every visitor a copy of; *element: its save element.
bool instanced(::Ctx* c, const State& s, uint64_t entry, uint64_t* element) {
    if (!guest::ok(entry, kEntrySize) || guest::rd<uint8_t>(entry + kEntryList) != kListImportant) return false;
    const uint64_t e = guest::rd<uint64_t>(entry);
    if (!guest::ok(e, kElementSize) || (guest::rd<uint8_t>(e + kElementFlags) & kFlagBagDrop)) return false;
    const uint32_t id = guest::rd<uint32_t>(e + kElementWord) & kItemIdMask;
    if (id - 1 >= kItemIdMax || (guest::call(c, s.base + kIsMemoryPiece, id) & 1)) return false;
    *element = e;
    return true;
}

std::string key_of(uint64_t element, uint8_t stage) {
    uint8_t place[16];
    std::memcpy(place, reinterpret_cast<const void*>(element + kElementPlace), sizeof place);
    return story_pickup_key(stage, guest::rd<uint32_t>(element + kElementWord), place);
}

void remember(State& s, const std::string& key) {
    if (!s.taken.insert(key).second) return;
    std::ofstream f(s.file, std::ios::binary | std::ios::app);
    if (f) f << key << '\n';
    if (!f && !s.write_failed) {
        s.write_failed = true;
        NXLOG_W(kTag, "Story co-op items: writing %s failed; items taken in this session are remembered until the game closes",
                save_files::u8(s.file).c_str());
    }
}

// 0x9cdba0(manager, entry, player, auto, forced), a visitor's: notes what the host gave, ends a try for an item already taken.
int on_pickup(::Ctx* c, State& s) {
    uint8_t stage = 0;
    const uint8_t visitor = layout::kSessionOnline | layout::kSessionVisitor;
    if ((session(s, &stage) & visitor) != visitor) return 0;
    const uint64_t manager = guest::rd<uint64_t>(s.base + layout::kPlayerManager);
    if (!guest::ok(manager, layout::kManagerPlayer + 8) || c->x[2] != guest::rd<uint64_t>(manager + layout::kManagerPlayer)) return 0;
    uint64_t element = 0;
    if (!instanced(c, s, c->x[1], &element)) return 0;
    const std::string key = key_of(element, stage);
    if (c->x[4] & 1) {
        remember(s, key);
        if (!s.logged_visitor) {
            s.logged_visitor = true;
            NXLOG_I(kTag, "Story co-op items: an important item taken as a visitor is taken once; noted in %s", save_files::u8(s.file).c_str());
        }
        return 0;
    }
    if (!s.taken.contains(key)) return 0;
    const double now = qpc::seconds();
    if (now - s.told_at > kNoticeSeconds) {
        s.told_at = now;
        notices::post("pkp.story_pickups", "Story co-op: you already took this item in a co-op world; it stays for the others", kNoticeSeconds);
    }
    return 1;
}

// Frees the entry's claim with the game's own call; the entry is then free whatever that call did.
void free_claim(::Ctx* c, State& s, uint64_t entry) {
    s.claim = guest::rd<uint8_t>(entry + kEntryClaim);
    if (s.claim != kClaimFree) guest::call(c, s.base + kClaim, entry, reinterpret_cast<uint64_t>(&s.claim), 0);
    if (guest::rd<uint8_t>(entry + kEntryClaim) != kClaimFree) guest::wr<uint8_t>(entry + kEntryClaim, kClaimFree);
}

// The host gave a visitor a memory piece (x21 = the entry, which stays in the world): the claim made for the visitor is freed.
void on_host_piece(::Ctx* c, State& s) {
    const uint8_t host = layout::kSessionOnline | layout::kSessionHost;
    if ((session(s, nullptr) & host) != host) return;
    const uint64_t entry = c->x[kPieceEntryReg];
    if (!guest::ok(entry, kEntrySize) || guest::rd<uint8_t>(entry + kEntryClaim) == kClaimFree) return;
    free_claim(c, s, entry);
    if (!s.logged_piece) {
        s.logged_piece = true;
        NXLOG_I(kTag, "Story co-op items: a memory piece a visitor takes stays free for the host to take");
    }
}

// The host's removal of an item it gave a visitor (x0 = the entry): the next call of 0x9cb680 is ended, the claim is freed.
void on_host_remove(::Ctx* c, State& s) {
    const uint8_t host = layout::kSessionOnline | layout::kSessionHost;
    s.keep = 0;
    if ((session(s, nullptr) & host) != host) return;
    const uint64_t entry = c->x[0];
    uint64_t element = 0;
    if (!instanced(c, s, entry, &element)) return;
    free_claim(c, s, entry);
    s.keep = entry;
    if (!s.logged_host) {
        s.logged_host = true;
        NXLOG_I(kTag, "Story co-op items: an important item a visitor takes stays in this world for the host and the other players");
    }
}

int hook(::Ctx* c, uint64_t, void* user) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return 0;
    switch (static_cast<SiteKind>(reinterpret_cast<uintptr_t>(user))) {
        case SiteKind::Pickup: return on_pickup(c, s);
        case SiteKind::HostRemove: on_host_remove(c, s); return 0;
        case SiteKind::HostPiece: on_host_piece(c, s); return 0;
        case SiteKind::Remove: {
            const uint64_t keep = s.keep;
            s.keep = 0;
            return keep && keep == c->x[0] ? 1 : 0;
        }
    }
    return 0;
}

void remove_hooks(State& s) {
    for (uint32_t& h : s.ids) {
        if (h && s.hooks) s.hooks->remove(h);
        h = 0;
    }
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.ids[0] || !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.keep = 0;
    s.taken.clear();
    std::error_code ec;
    const stdfs::path dir = save_files::save_dir() / kFolder;
    stdfs::create_directories(dir, ec);
    s.file = dir / (save_files::account_key() + ".tsv");
    std::ifstream f(s.file, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) s.taken.insert(line);
    }
    for (size_t i = 0; i < kSiteCount; ++i) {
        void* kind = reinterpret_cast<void*>(static_cast<uintptr_t>(kSites[i].kind));
        s.ids[i] = kSites[i].entry ? in.hooks.add_entry(in.base + kSites[i].site, &hook, kind) : in.hooks.add_mid(in.base + kSites[i].site, &hook, kind);
    }
}

// Play mode's frame (main is loaded): the first call checks every site's word (and that the midhook's is a hook site of the lifted
// game libraries) and arms the hooks or removes them.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.ids[0] || s.checked.exchange(true)) return;
    for (const Site& site : kSites) {
        const uint64_t a = s.base + site.site;
        uint32_t word = 0;
        if (mem::is_mapped(a, 4)) std::memcpy(&word, reinterpret_cast<const void*>(a), 4);
        if (word == site.word && (site.entry || fn_lookup(a) != nullptr)) continue;
        remove_hooks(s);
        NXLOG_W(kTag, "Story co-op items: off: main+0x%llx is not the expected hook site (word %08x, expected %08x)",
                static_cast<unsigned long long>(site.site), word, site.word);
        return;
    }
    s.armed.store(true, std::memory_order_release);
    NXLOG_I(kTag, "Story co-op items: every visitor gets a copy of a world's important items (%zu taken so far as a visitor)", s.taken.size());
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    for (uint32_t& h : s.ids) h = 0;
    s.hooks = nullptr;
    s.keep = 0;
}

}  // namespace

std::string story_pickup_key(uint8_t stage, uint32_t item_word, const uint8_t place[16]) {
    char b[64];
    int n = std::snprintf(b, sizeof b, "%u\t%06x\t", stage, item_word & kItemKeyMask);
    for (int i = 0; i < 16 && n > 0 && n + 2 < static_cast<int>(sizeof b); ++i) n += std::snprintf(b + n, sizeof b - n, "%02x", place[i]);
    return b;
}

const Feature kStoryPickupsFeature = {.name = "story_pickups", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
