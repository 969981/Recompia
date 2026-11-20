// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's hooks into the 2.0.1 main (pkp_coop.h).
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
    TalkKind,         // the talk handler's "is this the host" test: a member answers yes, so it gets the character's own talk (quest
                      // talks too) and not the visitor's stub
    QuestState,       // a story quest's state is set: every write comes from the quest pump's own steps, which every process runs on
                      // the flag alone, so nothing is sent; a write from anywhere else is logged once
    QuestFlag,        // a story quest's flag is set here: on a member in the session it does not run, and a step the member's own
                      // player made is asked of the authority (pkp_coop_quests.cpp); elsewhere logged (pkp_coop_story.cpp)
    QuestEffect,      // one effect command of a completed quest: world kinds run once, on the authority (the game's replication brings
                      // them to the others); personal kinds and story flags run on every member
    TownAction,       // a story line's step action 6 (the town register and house arrival, table 0x322d658 row 6): a member's own scene
                      // end leaves it to the authority, whose world is the group's; the line still steps
    WildPop,          // a wild creature's appearance armed or cleared (event-script command 0xc1e, quest effect 0x33): a member's goes
                      // to the authority, whose world is the group's
    AbilityEffect,    // quest effect 0x19's handler (an ability set or cleared): a clear is kept for the co-op run and shared, so the
                      // digest's merge never brings the ability back
    AbilityStep,      // the story-line step action that sets or clears an ability: a clear likewise
    NameEntry,        // event-script command 0x44, the name entry: after the talker's, every other member enters a name of their own
    ScriptItems,      // event-script command 0xbea, items given by a script: replayed for every member
    RewardItem,       // the item of a character's reward list (its talk reward): replayed for every member likewise; a member's
                      // reward talk runs the list on the host, so without this the member never got the item
    MaskStart,        // the permission-mask builder starts: its arguments are set so every stage type builds the full mask
    MaskPattern,      // the mask builder's online pattern: every member gets the full permissions with the story system on
    // ---- the requests ----
    GiverCheck,       // "can this creature have requests here" (the bind gate of the unique request manager): a member answers as at
                      // home, so the creatures of the group's world bind its own requests
    GoalCount,        // a unique request's completion step, right before its town goal +1: the run's one-off world effects are
                      // decided (once for the group), the goal skipped when they are not this run's
    Friendship,       // the friendship that completion step adds: skipped as decided at GoalCount
    GeneralGoal,      // a general request's completion, right before its town goal +1: a replay of another player's hand-in skips
                      // it, so each hand-in counts once
    // ---- the session ----
    PastTitle,        // the title screen's continue: the co-op save loads without the title, and its unclean-exit mark is cleared
    SimStep,          // the simulation step: skipped while the session waits for a dropped member (the network tick keeps running)
    TripFile,         // a trip is filed (five filers): every door out of the session is shut, except the stage moves the group makes
                      // together
    DropCheck,        // the drop check: a drop files no trip home; the session pauses and waits for the member instead
    AreaMove,         // the area move by destination index: in a session it waits for the group's poll (pkp_coop_travel.cpp)
    StateReset,       // the state reset after a drop: does not run
    Error3103,        // error 3103 (the session was lost): held back while the session waits for a reconnect
    LinkMenu,         // the PC's Link Play menu: shut, the co-op session is the launcher's
    MemberScreen,     // the member screen: shut likewise
    // ---- the save ----
    SaveWrite,        // a member's own save write (three writers): held, the authority writes the co-op save
    JoinBagSend,      // the host's side of a join's bag sync: it sends "no stored bag", so a member keeps its own bag instead of the
                      // host's stale copy (pkp_coop_bag.cpp)
    // ---- the shops ----
    ShopGrant,        // a purchase's give on the buyer's process (the shop's buy, or a visitor's buy the host answered): logged for the
                      // group, so every member gets it too (pkp_coop_purchases.cpp)
    VisitorSync,      // the visitor's "every Link Play request answered" test, from the event queue's ready test: a member's story events
                      // are not held for counters that stopped meeting (pkp_coop_held.cpp)
    RecordSend,       // the talk-end record's send: one with items the busy queue would drop is kept and sent later (pkp_coop_linkqueue.cpp)
    QueueReset,       // the visitor's request queue reset at session start: also clears a strict request an earlier session left out,
                      // which refused every talk-end record with items (pkp_coop_linkqueue.cpp)
    OfflineGoods,     // "this shop row is for offline play only" (the bag upgrades among them): no row is, in a session, so the shop
                      // lists them as it does offline (#780)
    // ---- the discoveries ----
    DexScene,         // the "registered in the Pokédex" scene is queued on the finder's process: sent, so every member registers
                      // the creature and sees the same scene (pkp_coop_discovery.cpp)
    HabitatScene,     // the "new habitat" scene is queued likewise: every member finds the habitat, with its scene
    // ---- the contents ----
    PlacedNotice,     // the "object placed" notice to the contents (from the placement): on a member it waits until the object is in
                      // the member's world, so the contents' "placed near a character" tests see it (pkp_coop_placement.cpp)
    PlayScene,        // a content's "play scene" command: a scene the event queue refuses (a talk on) is asked for again until it
                      // plays, so a one-shot record's scene is not lost (pkp_coop_contents.cpp)
};

struct CoopSite {
    HookSite at;
    Site kind;
};
constexpr CoopSite kSites[] = {
    {{0x3180e0, kPacibsp, true}, Site::TalkStart},
    {{0x3181cc, 0x2a1f03e0u, false}, Site::TalkNone},        // mov w0, wzr (after the talk event call 0x4bf7c0)
    {{0x47bd00, kPacibsp, true}, Site::TalkEnd},
    {{0x4c06d8, 0x2a000328u, false}, Site::TalkKind},        // orr w8, w25, w0 (w0 = IsHost's answer)
    {{0x5d865c, 0x51000428u, true}, Site::QuestState},       // sub w8, w1, #1
    {{0x5d8774, 0x51000428u, true}, Site::QuestFlag},        // sub w8, w1, #1
    {{0x11b214, 0x39400008u, true}, Site::QuestEffect},      // ldrb w8, [x0]
    {{0x1240dc, kPacibsp, true}, Site::WildPop},
    {{0x10c270, 0xf9400429u, true}, Site::TownAction},       // ldr x9, [x1, #8]
    {{0x1227fc, kPacibsp, true}, Site::AbilityEffect},
    {{0x10c0d0, kPacibsp, true}, Site::AbilityStep},
    {{0x43d3e0, kPacibsp, true}, Site::NameEntry},
    {{0x12c734, 0x79400808u, true}, Site::ScriptItems},      // ldrh w8, [x0, #4]
    {{0x3bcbc0, 0xaa0203e8u, true}, Site::RewardItem},       // mov x8, x2
    {{0xb0f74, 0xaa0003f3u, false}, Site::MaskStart},        // mov x19, x0
    {{0xb1030, 0xf2c0002du, false}, Site::MaskPattern},      // movk x13, #1, lsl #32
    {{0x11435c, kPacibsp, true}, Site::GiverCheck},
    {{0x61858c, 0xb4000040u, false}, Site::GoalCount},       // cbz x0, 0x618594 (x0 = the challenge manager, before bl 0x618b00)
    {{0x619410, kPacibsp, true}, Site::Friendship},
    {{0x621be8, 0xb4000040u, false}, Site::GeneralGoal},     // cbz x0, 0x621bf0 (x0 = the challenge manager, before bl 0x618b00)
    {{0xa88214, 0xb00156c8u, true}, Site::PastTitle},        // adrp x8, ...
    {{0x231890, kPacibsp, true}, Site::SimStep},
    {{0xbd45c, 0xb9400008u, true}, Site::TripFile},          // ldr w8, [x0]
    {{0xbdc68, kPacibsp, true}, Site::TripFile},
    {{0xbdd28, kPacibsp, true}, Site::TripFile},
    {{0x5483a0, kPacibsp, true}, Site::TripFile},
    {{0xbe5f0, kPacibsp, true}, Site::TripFile},
    {{0xbca8c, kPacibsp, true}, Site::DropCheck},
    {{0xbda20, kPacibsp, true}, Site::AreaMove},
    {{0xb0d74, kPacibsp, true}, Site::StateReset},
    {{0x547278, kPacibsp, true}, Site::Error3103},
    {{0xf282a8, kPacibsp, true}, Site::LinkMenu},
    {{0xfb9ea0, kPacibsp, true}, Site::MemberScreen},
    {{0x241324, kPacibsp, true}, Site::SaveWrite},
    {{0x245200, kPacibsp, true}, Site::SaveWrite},
    {{0x5a8a34, kPacibsp, true}, Site::SaveWrite},
    {{0x5728b4, 0xf9400660u, false}, Site::JoinBagSend},     // ldr x0, [x19, #8] (before the found flag is written)
    {{0xe889f8, kPacibsp, true}, Site::ShopGrant},
    {{0xe87260, kPacibsp, true}, Site::OfflineGoods},
    {{0x3b9060, kPacibsp, true}, Site::DexScene},
    {{0x45f2a0, 0x12003c28u, true}, Site::HabitatScene},     // and w8, w1, #0xffff
    {{0x313418, kPacibsp, true}, Site::VisitorSync},
    {{0x2159c0, 0x90019a68u, true}, Site::RecordSend},       // adrp x8, 0x3561000
    {{0x214954, 0xb0019a68u, true}, Site::QueueReset},       // adrp x8, 0x3561000
    {{0x13596c, 0x12003c29u, true}, Site::PlacedNotice},     // and w9, w1, #0xffff
    {{0x12b544, 0xb9400408u, true}, Site::PlayScene},        // ldr w8, [x0, #4]
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
        case Site::QuestFlag: return coop::quests::on_quest_flag(c) ? 1 : coop::story::on_quest_flag(c);
        case Site::QuestEffect: return coop::story::on_quest_effect(c);
        case Site::WildPop: return coop::story::on_wild_pop(c);
        case Site::TownAction: return coop::story::on_town_action(c);
        case Site::AbilityEffect: return coop::story::on_ability_effect(c);
        case Site::AbilityStep: return coop::story::on_ability_step(c);
        case Site::TalkStart: return coop::talk::on_talk_start(c);
        case Site::TalkNone: return coop::talk::on_no_talk(c);
        case Site::TalkEnd: return coop::talk::on_talk_end(c);
        case Site::TalkKind: return coop::talk::on_talk_kind(c);
        case Site::NameEntry: return coop::catchup::on_name_entry(c);
        case Site::ScriptItems: return coop::catchup::on_script_items(c);
        case Site::RewardItem: return coop::catchup::on_reward_item(c);
        case Site::GiverCheck: return coop::requests::on_giver_check(c);
        case Site::GoalCount: return coop::outcomes::on_goal_count(c);
        case Site::Friendship: return coop::outcomes::on_friendship(c);
        case Site::GeneralGoal: return coop::requests::on_general_goal(c);
        case Site::AreaMove: return coop::travel::on_area_move(c);
        case Site::ShopGrant: return coop::purchases::on_grant(c);
        case Site::OfflineGoods: return coop::purchases::on_offline_only(c);
        case Site::JoinBagSend: return coop::bag::on_join_bag_send(c);
        case Site::DexScene: return coop::discovery::on_dex_scene(c);
        case Site::HabitatScene: return coop::discovery::on_habitat_scene(c);
        case Site::VisitorSync: return coop::held::on_visitor_sync(c);
        case Site::RecordSend: return coop::linkqueue::on_send_record(c);
        case Site::QueueReset: return coop::linkqueue::on_session_reset(c);
        case Site::PlacedNotice: return coop::placement::on_placed_notice(c);
        case Site::PlayScene: return coop::contents::on_play_scene(c);
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
    launch.members = static_cast<int>(settings::get_int(settings::Key::CoopMembers));
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
    coop::quests::tick(c);
    coop::requests::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::outcomes::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::recipes::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::events::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::discovery::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::unlocks::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::purchases::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::talk::tick();
    coop::travel::tick(c);
    coop::catchup::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::linkqueue::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::notice::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::placement::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::contents::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::gate::tick(s.in_game.load(std::memory_order_relaxed));
    coop::trace::tick(s.in_game.load(std::memory_order_relaxed));
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
    coop::quests::reset();
    coop::requests::reset();
    coop::outcomes::reset();
    coop::recipes::reset();
    coop::events::reset();
    coop::discovery::reset();
    coop::unlocks::reset();
    coop::purchases::reset();
    coop::talk::reset();
    coop::travel::reset();
    coop::catchup::reset();
    coop::notice::reset();
    coop::curtain::reset();
    coop::gate::reset();
    coop::trace::reset();
    coop::held::reset();
    coop::linkqueue::reset();
    coop::placement::reset();
    coop::contents::reset();
    coop::session::reset();
}

}  // namespace

namespace coop {

uint64_t base() { return st().base; }
CoopRole role() { return st().launch.role; }
int members() { return st().launch.members; }
const std::string& member_key() { return st().launch.member; }

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

const Feature kCoopFeature = {.name = "coop",
                              .install = &install,
                              .remove = &remove,
                              .frame = &frame,
                              .game_frame = &game_frame,
                              .overlay_active = &coop::curtain::overlay_active,
                              .overlay_draw = &coop::curtain::overlay_draw};

}  // namespace nxrt::games::pkp
