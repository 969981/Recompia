// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op's hooks into the 2.0.1 main (pkp_coop.h).
//
// Every site the co-op phases need is registered here, in coop mode only, so the game libraries are lifted once with all of them
// (recomp/games/pkp/main.sites). Each hook hands its site to the part of co-op that owns it (pkp_coop_internal.h); the comment of each
// site says what that part does there. The session and save sites pass through until their parts land (returns 0: the game's own code
// runs). The sites are checked once main is loaded (pkp_site_check.h): one that is
// not the expected instruction, or a midhook site the lifted libraries lack, removes every co-op hook.
#include "core/pkp_coop.h"
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_site_check.h"

#include <atomic>
#include <cctype>
#include <cstddef>

#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";
constexpr size_t kMemberDigits = 32;

constexpr uint32_t kPacibsp = 0xd503237fu;   // the first instruction of most of the game's functions

// What the game does at a site; the hook's user value.
enum class Site : uintptr_t {
    // ---- the shared story ----
    TalkStart,        // the talk start: the talk lock for that character is asked of the authority; refused, the player gets the busy
                      // line and no talk starts
    TalkNone,         // the talk start right after its talk event call 0x4bf7c0 returns (every return): w22 bit 0 clear there is the
                      // "no talk after all" path (the function then returns 0), where the lock is given back
    TalkEnd,          // the end of a talk's event chain: the lock of that talk slot is given back
    QuestState,       // a story quest's state is set: a local write goes to the authority, which orders it and sends it to every member;
                      // a member's copy arriving is applied through this same setter, marked so it is not sent again
    QuestFlag,        // a story quest's flag is set: as QuestState
    AbilityEffect,    // an ability is given (two effect functions): sent to the others as a story change
    QuestEffect,      // one effect command of a completed quest: world kinds run once, on the authority (the game's replication brings
                      // them to the others); personal kinds and story flags run on every member
    NameEntry,        // event-script command 0x44, the name entry: after the talker's, every other member enters a name of their own
    ScriptItems,      // event-script command 0xbea, items given by a script: replayed for every member
    MaskStart,        // the permission-mask builder starts: its arguments are set so every stage type builds the full mask
    MaskPattern,      // the mask builder's online pattern: every member gets the full permissions with the story system on
    // ---- the session ----
    PastTitle,        // the title screen's continue: the co-op save loads without the title, and its unclean-exit mark is cleared
    SimStep,          // the simulation step: skipped while the session waits for a dropped member (the network tick keeps running)
    TripFile,         // a trip is filed (five filers): every door out of the session is shut, except the stage moves the group makes
                      // together
    DropCheck,        // the drop check: a drop files no trip home; the session pauses and waits for the member instead
    StateReset,       // the state reset after a drop: does not run
    Error3103,        // error 3103 (the session was lost): held back while the session waits for a reconnect
    LinkMenu,         // the PC's Link Play menu: shut, the co-op session is the launcher's
    MemberScreen,     // the member screen: shut likewise
    // ---- the save ----
    SaveWrite,        // a member's own save write (three writers): held, the authority writes the co-op save
    JoinBagSync,      // the bag sync of a join: the authority's working copy never overwrites a member's own bag
};

struct CoopSite {
    HookSite at;
    Site kind;
};
constexpr CoopSite kSites[] = {
    {{0x3180e0, kPacibsp, true}, Site::TalkStart},
    {{0x3181cc, 0x2a1f03e0u, false}, Site::TalkNone},        // mov w0, wzr (after the talk event call 0x4bf7c0)
    {{0x47bd00, kPacibsp, true}, Site::TalkEnd},
    {{0x5d865c, 0x51000428u, true}, Site::QuestState},       // sub w8, w1, #1
    {{0x5d8774, 0x51000428u, true}, Site::QuestFlag},        // sub w8, w1, #1
    {{0x121bb8, kPacibsp, true}, Site::AbilityEffect},
    {{0x1227fc, kPacibsp, true}, Site::AbilityEffect},
    {{0x11b214, 0x39400008u, true}, Site::QuestEffect},      // ldrb w8, [x0]
    {{0x43d3e0, kPacibsp, true}, Site::NameEntry},
    {{0x12c734, 0x79400808u, true}, Site::ScriptItems},      // ldrh w8, [x0, #4]
    {{0xb0f74, 0xaa0003f3u, false}, Site::MaskStart},        // mov x19, x0
    {{0xb1030, 0xf2c0002du, false}, Site::MaskPattern},      // movk x13, #1, lsl #32
    {{0xa88214, 0xb00156c8u, true}, Site::PastTitle},        // adrp x8, ...
    {{0x231890, kPacibsp, true}, Site::SimStep},
    {{0xbd45c, 0xb9400008u, true}, Site::TripFile},          // ldr w8, [x0]
    {{0xbdc68, kPacibsp, true}, Site::TripFile},
    {{0xbdd28, kPacibsp, true}, Site::TripFile},
    {{0x5483a0, kPacibsp, true}, Site::TripFile},
    {{0xbe5f0, kPacibsp, true}, Site::TripFile},
    {{0xbca8c, kPacibsp, true}, Site::DropCheck},
    {{0xb0d74, kPacibsp, true}, Site::StateReset},
    {{0x547278, kPacibsp, true}, Site::Error3103},
    {{0xf282a8, kPacibsp, true}, Site::LinkMenu},
    {{0xfb9ea0, kPacibsp, true}, Site::MemberScreen},
    {{0x241324, kPacibsp, true}, Site::SaveWrite},
    {{0x245200, kPacibsp, true}, Site::SaveWrite},
    {{0x5a8a34, kPacibsp, true}, Site::SaveWrite},
    {{0x5727c0, kPacibsp, true}, Site::JoinBagSync},
};
constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t ids[kSiteCount] = {};   // all 0: not coop mode, or not installed
    CoopLaunch launch;
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    std::atomic<bool> in_game{false};   // play mode's, from its frame (main thread) for the game-frame tick
};
State& st() {
    static State s;
    return s;
}

// Every co-op site (user: its Site), once the frame callback armed the hooks: the part of co-op that owns the site does its work there.
// A site no part owns yet passes through (returns 0: the game's own code runs).
int hook(::Ctx* c, uint64_t, void* user) {
    if (!st().armed.load(std::memory_order_acquire)) return 0;
    switch (static_cast<Site>(reinterpret_cast<uintptr_t>(user))) {
        case Site::MaskStart: return coop::story::on_mask_start(c);
        case Site::MaskPattern: return coop::story::on_mask_pattern(c);
        case Site::QuestState: return coop::story::on_quest_state(c);
        case Site::QuestFlag: return coop::story::on_quest_flag(c);
        case Site::AbilityEffect: return coop::story::on_ability(c);
        case Site::QuestEffect: return coop::story::on_quest_effect(c);
        case Site::TalkStart: return coop::talk::on_talk_start(c);
        case Site::TalkNone: return coop::talk::on_no_talk(c);
        case Site::TalkEnd: return coop::talk::on_talk_end(c);
        case Site::NameEntry: return coop::catchup::on_name_entry(c);
        case Site::ScriptItems: return coop::catchup::on_script_items(c);
        default: return 0;
    }
}

void remove_hooks(State& s) {
    for (uint32_t& h : s.ids) {
        if (h && s.hooks) s.hooks->remove(h);
        h = 0;
    }
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.ids[0] || !settings::get_bool(settings::Key::Coop)) return;
    const char* why = nullptr;
    CoopLaunch launch;
    if (!coop_launch_parse(settings::get_str(settings::Key::CoopRole), settings::get_str(settings::Key::CoopGroup),
                           settings::get_str(settings::Key::CoopMember), &launch, &why)) {
        NXLOG_W(kTag, "Co-op ([pkp] coop): off, the launch's co-op facts are not usable: %s", why);
        return;
    }
    s.launch = launch;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    for (size_t i = 0; i < kSiteCount; ++i) {
        const HookSite& at = kSites[i].at;
        void* kind = reinterpret_cast<void*>(static_cast<uintptr_t>(kSites[i].kind));
        s.ids[i] = at.entry ? in.hooks.add_entry(in.base + at.site, &hook, kind) : in.hooks.add_mid(in.base + at.site, &hook, kind);
    }
    NXLOG_I(kTag, "Co-op ([pkp] coop): on, %s of group %s, %zu hooks, main is checked once the game runs",
            launch.role == CoopRole::Host ? "host" : "visit", launch.group.c_str(), kSiteCount);
}

// Play mode's frame (main is loaded): the first call checks the sites and arms the hooks or removes them.
void frame(bool in_game, double, play::GameFrame*) {
    State& s = st();
    s.in_game.store(in_game, std::memory_order_relaxed);
    if (!s.ids[0] || s.checked.exchange(true)) return;
    HookSite at[kSiteCount];
    for (size_t i = 0; i < kSiteCount; ++i) at[i] = kSites[i].at;
    uint32_t word = 0;
    if (const HookSite* bad = first_bad_site(s.base, at, kSiteCount, &word)) {
        remove_hooks(s);
        NXLOG_W(kTag, "Co-op: off: main+0x%llx is not the expected hook site (word %08x, expected %08x)",
                static_cast<unsigned long long>(bad->site), word, bad->word);
        return;
    }
    s.armed.store(true, std::memory_order_release);
    NXLOG_I(kTag, "Co-op: armed (%zu sites)", kSiteCount);
}

// The game thread, once a frame: the parts' per-frame work, in this order.
void game_frame(::Ctx* c) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return;
    coop::session::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::link::tick(c);
    coop::story::tick(c);
    coop::talk::tick();
    coop::catchup::tick(c, s.in_game.load(std::memory_order_relaxed));
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    s.in_game.store(false);
    for (uint32_t& h : s.ids) h = 0;
    s.hooks = nullptr;
    s.launch = {};
    coop::link::reset();
    coop::story::reset();
    coop::talk::reset();
    coop::catchup::reset();
    coop::session::reset();
}

}  // namespace

namespace coop {

uint64_t base() { return st().base; }
CoopRole role() { return st().launch.role; }

uint8_t session_bits() {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(game, layout::kStateSession + 1) ? guest::rd<uint8_t>(game + layout::kStateSession) : 0;
}

uint64_t local_player() {
    const uint64_t players = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!guest::ok(players, layout::kManagerPlayer + 8)) return 0;
    const uint64_t me = guest::rd<uint64_t>(players + layout::kManagerPlayer);
    return guest::ok(me, layout::kPosition + 16) ? me : 0;
}

}  // namespace coop

bool coop_launch_parse(const std::string& role, const std::string& group, const std::string& member, CoopLaunch* out, const char** why) {
    if (role == "host") out->role = CoopRole::Host;
    else if (role == "visit") out->role = CoopRole::Visit;
    else {
        *why = "coop_role is neither host nor visit";
        return false;
    }
    if (group.empty()) {
        *why = "coop_group is empty";
        return false;
    }
    if (member.size() != kMemberDigits) {
        *why = "coop_member is not 32 hex digits";
        return false;
    }
    out->member.clear();
    for (char ch : member) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            *why = "coop_member is not 32 hex digits";
            return false;
        }
        out->member += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    out->group = group;
    return true;
}

const Feature kCoopFeature = {.name = "coop", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
