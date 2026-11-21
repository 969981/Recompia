// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's hooks into the 2.0.1 main (pkp_coop.h).
//
// Every site the co-op phases need is registered here, in coop mode only, so the game libraries are lifted once with all of them
// (recomp/games/pkp/main.sites). Each hook hands its site to the part of co-op that owns it (pkp_coop_internal.h); the comment of each
// site says what that part does there. The sites marked "reserved" have no part yet and pass through (returns 0: the game's own code
// runs). The sites are checked once main is loaded (pkp_site_check.h): one that is
// not the expected instruction, or a midhook site the lifted libraries lack, removes every co-op hook.
#include "core/pkp_coop.h"
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_site_check.h"

#include <atomic>
#include <cctype>
#include <cstddef>
#include <optional>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";
constexpr size_t kMemberDigits = 32;
constexpr const char* kOffNotice = "pkp.coop";   // the notice of co-op turning itself off at the site check
constexpr double kOffNoticeSeconds = 12.0;       // long enough to read; shown at start-up and again at the first gameplay frame


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
                      // it, so each hand-in counts once; the talker's own hand-in shares its reward with the group
    CleanCount,       // the sludge cleaning count's online gate (Lapras's request): the authority counts the group's cleanings
    // ---- the session ----
    PastTitle,        // the title's result getter: while the title waits for a press, the co-op save continues with no press, and its
                      // "did not disconnect correctly" mark is cleared (pkp_coop_title.cpp)
    SimStep,          // the simulation step 0x231890: skipped while the group waits for a dropped member and nothing is in flight
                      // (pkp_coop_drop.cpp); the network and the menus run on
    TripFile,         // the trip filers (0xbd45c, 0xbdc68, 0xbdd28, the kick 0x5483a0, the host's StageMove 0xbe5f0): Save and Quit
                      // and the group's moves run (a member's leave and the host's close are announced first); the kick's "go
                      // home" filing is refused in a session (pkp_coop_drop.cpp)
    DropCheck,        // the drop check 0xbca8c: not handled, the game's own code runs (a process whose session ended is sent home,
                      // and pkp_coop_session.cpp joins again; pkp_coop_drop.cpp keeps that trip). It is entered every frame the trip
                      // record is idle (0xbaf54's tail call) and decides inside
    AreaMove,         // the area move by destination index: in a session it waits for the group's poll (pkp_coop_travel.cpp)
    GateEntry,        // an area gate's entry (its passage): in a session it waits for the group's poll, before anything fades
    Error3103,        // the online error display 0x547278: error 3103 (the session was lost) is held back in a co-op session and its
                      // rejoin window says it instead (pkp_coop_drop.cpp)
    LinkMenu,         // the PC's Link Play screen's state entry: in a session, a state that makes a session call is refused, the
                      // co-op session is the launcher's (pkp_coop_doors.cpp)
    PauseMenu,        // the pause menu's state entry: in a session, the host's "send this member home" and a member's rejection are
                      // refused; Save and Quit (the host closes the session, a teammate leaves it) stays (pkp_coop_doors.cpp)
    // ---- the save ----
    SaveWrite,        // a save write (0x241324, 0x245200, 0x5a8a34): reserved for holding a member's own writes once the authority
                      // writes the shared co-op save; not handled yet, the game's own code runs (every member saves its own copy)
    // ---- the bags: each member's is a record of ours (pkp_coop_bags.cpp); the game's writers that fight it are off (pkp_coop_bag.cpp) ----
    JoinBagSend,      // the host's side of a join's bag sync: it sends "no stored bag", so a member keeps its own bag instead of the
                      // host's stale copy
    MirrorFill,       // the host's fill of a joining visitor's mirror from its per-user table: the mirror gets the member's record
    StageBagClear,    // the stage load's clear of the online bag (Cloud Island, a visitor in a session): skipped
    BagToStorage,     // the stage load's step and the GameShare end that move the bag into the PC: skipped, the share bit cleared
    HostBagStore,     // the host's moves between its per-user table and a bag (Cloud Island's own bag, the pre-save store): skipped
    RefusalReset,     // a member's bag reset to the host's copy when the host refused a request: the reset is skipped
    RefusalResetWait, // the same in kind 0xb's reply, whose skipped path also releases the request's wait (done by the hook)
    BagScreen,        // the bag / container screen's frame: while it runs, an apply of the record waits
    // ---- the shops ----
    ShopGrant,        // a purchase's give on the buyer's process (the shop's buy, or a visitor's buy the host answered): logged for the
                      // group, so every member gets it too (pkp_coop_purchases.cpp)
    VisitorSync,      // the visitor's "every Link Play request answered" test, from the event queue's ready test: a member's story events
                      // are not held for counters that stopped meeting (pkp_coop_held.cpp)
    RecordSend,       // the talk-end record's send: one with items the busy queue would drop is kept and sent later (pkp_coop_linkqueue.cpp)
    QueueReset,       // the visitor's request queue reset at session start: also clears a strict request an earlier session left out,
                      // which refused every talk-end record with items (pkp_coop_linkqueue.cpp)
    ChallengePay,     // the coins of a claimed challenge: on a member (the host's goal update pays it) only once per challenge, the
                      // ledger pkp_coop_money.cpp also pays the group's claims from
    OfflineGoods,     // "this shop row is for offline play only" (the bag upgrades among them): no row is, in a session, so the shop
                      // lists them as it does offline (#780)
    ShopTile,         // the PC menu's "a visitor may open the shop" test: a member opens it, which the member mask's cleared bit 23
                      // (no per-row visitor lock) would otherwise grey out
    StockGet,         // the shop's stock left, on a member: the own record, which holds the authority's numbers (pkp_coop_stock.cpp)
    StockSet,         // the shop's stock set, on a member: the own record, and the count taken goes to the authority
    Restock,          // the daily lineup restock, on a member: not run (the authority's reaches it)
    // ---- the PC menu's locks (pkp_coop_pclock.cpp) ----
    PaneNotice,       // the kind-28 notice of the PC's Shop / challenges pane: a lock still set after it is ended here
    // ---- the discoveries ----
    DexScene,         // the "registered in the Pokédex" scene is queued on the finder's process: sent, so every member registers
                      // the creature and sees the same scene (pkp_coop_discovery.cpp)
    HabitatScene,     // the "new habitat" scene is queued likewise: every member finds the habitat, with its scene
    // ---- the contents ----
    PlacedNotice,     // the "object placed" notice to the contents (from the placement): on a member it waits until the object is in
                      // the member's world, so the contents' "placed near a character" tests see it (pkp_coop_placement.cpp)
    PlayScene,        // a content's "play scene" command: a scene the event queue refuses (a talk on) is asked for again until it
                      // plays, so a one-shot record's scene is not lost (pkp_coop_contents.cpp)
    // ---- the unlocks ----
    AreaGateRecord,   // the area gate record's "a session is open" guard: the write goes through, so a gate opened in a session can be
                      // entered, and is shared with the group (pkp_coop_unlocks.cpp)
    // ---- the ability check (pkp_coop_story.cpp) ----
    AbilityCheck,     // the ability check's "a visitor reads the host's record" branch: a member reads its own bits, which hold the
                      // group's abilities (the Environment Level panel on a teammate's PC menu)
    // ---- the role answers (pkp_coop_role.cpp) ----
    RoleAnswer,       // a role getter's call listed in pkp_coop_role_table.inc (a permission or display check): a member gets the host's
                      // answer there, every other caller the game's own
    // ---- the creature jobs (pkp_coop_jobs.cpp) ----
    JobTick,          // the item-making tick's IsMine test: the authority also ticks a job on a creature a member holds, which
                      // otherwise no process ticks
    // ---- friendship (pkp_coop_friendship.cpp) ----
    FriendGain,       // a friendship gain with a creature: a member's (outside Cloud Island) is made on the authority, whose record
                      // is the group's
    FriendFlag,       // script callback slot 175's friend record flag: a member's is set on the authority too
    FriendTag,        // the name tag's friendship test: a member reads its copy of the host's records, as the host reads its own
    // ---- the challenges: per-player claims (pkp_coop_money.cpp), a member's actions counted (pkp_coop_challenges.cpp) ----
    ClaimVisitor,     // the PC's claim after its visitor test: a member's own claim pays it its coins (kPay's ledger keeps it to once)
    BuyCount,         // the "buy items" challenge count after its visitor test: a member's buy counts for the group
    // ---- the creature events (pkp_coop_feed.cpp) ----
    TalkedTo,         // the contents scheduler's "talked to" notice, which skips a visitor: a member's talk is posted on the
                      // authority, so content records waiting on that talk run for the group
    Fed,              // the "fed" notice of a field feed (held-item command kind 9), which skips a visitor: a member's feed is made
                      // on the authority too, so the group's creature eats it
    // ---- a member's talk gift (pkp_coop_friendship.cpp) ----
    FriendGift,       // the host's apply of a member's talk gift: its friendship is made on the authority outside Cloud Island too
    // ---- gimmick ownership (pkp_coop_owner.cpp) ----
    UseRefused,       // a gimmick press a story content took instead of the use: the object is given back, as when the class's
                      // event does not start
    // ---- a newly met creature moving in (pkp_coop_newcomer.cpp) ----
    Befriend,         // the talk end step's befriend call: a member's first talk with a new creature is made on the authority
    // ---- a shared object a member was using when it left (pkp_coop_gimmicks.cpp) ----
    SharedLeave,      // a multi-user object's own "member left" step (MemberDelete): the character the leaver had at it is let go too
    // ---- a member's later talks with a resident (pkp_coop_talk.cpp) ----
    TalkOwn,          // the talk handler's "this player is the creature's own" test: a member is, for a creature living in the
                      // town, so its talks go on to the host's later stages instead of the stranger's talk
    // ---- gimmick ownership, a grant that starts no use (pkp_coop_owner.cpp) ----
    GrantNoUse,       // a member's ownership grant after which no use runs (its data apply vt+0x2f8 failed, or the class runs none
                      // from a grant): the object is given back
    // ---- gimmick ownership, the host's own use of the story's materials drop ends (pkp_coop_owner.cpp) ----
    HostUseEnd,       // class 0x326dc98's close: the host's own use is given back, which nothing in the class does
    // ---- the talk conditions inside a member's talk (pkp_coop_talk.cpp) ----
    TalkCondParent,   // talk condition 0x52's role test: a member answers as the host, from the group's record of the creature
    TalkCondVisitor,  // talk condition 0x25 (IsVisitor): false for a member, so it gets no visitor-only talk the host would not
    // ---- a gimmick's release on a member's busy request queue, the ownership log (pkp_coop_linkqueue.cpp, pkp_coop_owner.cpp) ----
    ReleaseGate,      // a close's "every request answered" test before its release record: let through, the record is kept instead
    RequestSend,      // the generic request send: a release record the busy queue would refuse is kept and sent once it is idle
    OwnerApply,       // the ownership apply (take / release): logged; a member's new take drops a kept release of the same object
    OwnerPress,       // the player's press on an object: remembered for the busy line's log
    OwnerNotice,      // the message log's post: "Someone else is using this right now." logged with the object
    OwnerRefused,     // a member's grant reply: a take the host turned down is logged
    // ---- a member's take-out of its Ordered Items and Gifts (pkp_coop_takeout.cpp) ----
    TakeoutStore,     // the shared move's choice of the store a taken item is removed from: the take-out screen's removal of a
                      // member's ordered item or gift uses the own store it listed, not the visitor's temp list
    // ---- a member's deposit into the story's materials drop (pkp_coop_deposit.cpp) ----
    DepositStore,     // the container slice: a member's drop slice is its own store, so its screen, check and our clear share one
    DepositConfirm,   // the drop's close after the screen's confirm: a member's deposit is stepped on the authority, not here
    // ---- a story follow on a member (pkp_coop_follow.cpp) ----
    StoryFollow,      // a story content's "follow this player" attach (0x2a1ed4, local only): on a member it does not run and the
                      // authority makes it for its own copy of the creature, so no member takes the host's follow away
    // ---- a creature's talk record flag a member's talk arms (pkp_coop_talkrec.cpp) ----
    TalkRecArmed,     // script callback 195's set of the talk record's +0x10: a member's is set in the authority's record too
    // ---- the Star Piece swap in a member's talk (pkp_coop_talk.cpp) ----
    TalkSwapParent,   // script callback slot 119's parent term: a member's is the group's record of the creature, as for the host
    // ---- an event's end on a member (pkp_coop_feed.cpp) ----
    EventEnded,       // the contents notifier, called by the three "event ended" notifiers: a member's is posted on the authority too,
                      // so content records waiting on a scene a member played run for the group
    // ---- a content's scene refused by a character another player holds (pkp_coop_contents.cpp) ----
    SceneTake,        // the event setup's "every character is ready" test: a remembered scene refused long enough starts anyway
    SceneStart,       // the event start's IsMine of one character: the same scene takes a character another player holds
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
    {{0x6522ec, 0x36000080u, false}, Site::CleanCount},      // tbz w0, #0, 0x6522fc (w0 = IsOnline's answer) in 0x6520e0
    {{0x652940, 0x36000080u, false}, Site::CleanCount},      // tbz w0, #0, 0x652950 in 0x652880
    {{0xa88214, 0xb00156c8u, true}, Site::PastTitle},        // adrp x8, ...
    {{0x231890, kPacibsp, true}, Site::SimStep},
    {{0xbd45c, 0xb9400008u, true}, Site::TripFile},          // ldr w8, [x0]
    {{0xbdc68, kPacibsp, true}, Site::TripFile},
    {{0xbdd28, kPacibsp, true}, Site::TripFile},
    {{0x5483a0, kPacibsp, true}, Site::TripFile},
    {{0xbe5f0, kPacibsp, true}, Site::TripFile},
    {{0xbca8c, kPacibsp, true}, Site::DropCheck},
    {{0xbda20, kPacibsp, true}, Site::AreaMove},
    {{0x7b0d80, kPacibsp, true}, Site::GateEntry},
    {{0x547278, kPacibsp, true}, Site::Error3103},
    {{0xf282a8, kPacibsp, true}, Site::LinkMenu},
    {{0xfb9ea0, kPacibsp, true}, Site::PauseMenu},
    {{0x241324, kPacibsp, true}, Site::SaveWrite},
    {{0x245200, kPacibsp, true}, Site::SaveWrite},
    {{0x5a8a34, kPacibsp, true}, Site::SaveWrite},
    {{0x5728b4, 0xf9400660u, false}, Site::JoinBagSend},     // ldr x0, [x19, #8] (before the found flag is written)
    {{0x575644, kPacibsp, true}, Site::MirrorFill},
    {{0x5afee0, kPacibsp, true}, Site::StageBagClear},
    {{0x5affe0, kPacibsp, true}, Site::BagToStorage},
    {{0x5b0d60, kPacibsp, true}, Site::BagToStorage},
    {{0x5b0a60, kPacibsp, true}, Site::HostBagStore},
    {{0x5af6a0, kPacibsp, true}, Site::HostBagStore},
    {{0x2288c4, 0xb40002a0u, false}, Site::RefusalResetWait}, // cbz x0 (x0 = the selected bag) in 0x226280's kind 0xb reply
    {{0x228f38, 0xb4000620u, false}, Site::RefusalReset},     // the same, kind 0xf
    {{0x22a298, 0xb40002c0u, false}, Site::RefusalReset},     // kind 0x1a
    {{0x2267fc, 0xb4000660u, false}, Site::RefusalReset},     // kind 0x1d
    {{0x226684, 0xb4001220u, false}, Site::RefusalReset},     // kind 0x1e
    {{0x226868, 0xb4000300u, false}, Site::RefusalReset},     // kind 0x1f
    {{0xfeee78, kPacibsp, true}, Site::BagScreen},
    {{0xe889f8, kPacibsp, true}, Site::ShopGrant},
    {{0xe87260, kPacibsp, true}, Site::OfflineGoods},
    {{0xe8d428, kPacibsp, true}, Site::ShopTile},
    {{0x646864, kPacibsp, true}, Site::StockGet},
    {{0x646ae8, kPacibsp, true}, Site::StockSet},
    {{0x632380, kPacibsp, true}, Site::Restock},
    {{0x1364f4, kPacibsp, true}, Site::PaneNotice},
    {{0x637480, kPacibsp, true}, Site::ChallengePay},
    {{0x3b9060, kPacibsp, true}, Site::DexScene},
    {{0x45f2a0, 0x12003c28u, true}, Site::HabitatScene},     // and w8, w1, #0xffff
    {{0x313418, kPacibsp, true}, Site::VisitorSync},
    {{0x2159c0, 0x90019a68u, true}, Site::RecordSend},       // adrp x8, 0x3561000
    {{0x214954, 0xb0019a68u, true}, Site::QueueReset},       // adrp x8, 0x3561000
    {{0x13596c, 0x12003c29u, true}, Site::PlacedNotice},     // and w9, w1, #0xffff
    {{0x12b544, 0xb9400408u, true}, Site::PlayScene},        // ldr w8, [x0, #4]
    {{0x5c05c0, 0x37000140u, false}, Site::AreaGateRecord},  // tbnz w0, #0, 0x5c05e8 (w0 = IsOnline's answer)
    // the ability check (pkp_coop_story.cpp)
    {{0x1140e8, 0x3617fb08u, false}, Site::AbilityCheck},    // tbz w8, #2, 0x114048 (w8 = GM+0x90, the session bits)
    // ---- the role answers (pkp_coop_role.cpp): one midhook per row (after its `bl`, or at its session-bit test) ----
#define PKP_ROLE_ROW(ret, getter, word, tag, what) {{ret, word, false}, Site::RoleAnswer},
#define PKP_ROLE_BIT(site, reg, bit, word, tag, what) {{site, word, false}, Site::RoleAnswer},
#include "core/pkp_coop_role_table.inc"
#undef PKP_ROLE_BIT
#undef PKP_ROLE_ROW
    // the creature jobs (pkp_coop_jobs.cpp)
    {{0x3b39a8, 0x36001540u, false}, Site::JobTick},         // tbz w0, #0, 0x3b3c50 (w0 = IsMine's answer)
    // friendship
    {{0x3a89fc, kPacibsp, true}, Site::FriendGain},
    {{0x4ab358, 0x36000120u, false}, Site::FriendFlag},      // tbz w0, #0, 0x4ab37c (w0 = Cloud Island, w19 = the chara)
    {{0x3b7cd0, 0x36000540u, false}, Site::FriendTag},       // tbz w0, #0, 0x3b7d78 (w0 = Cloud Island, on a visitor)
    // the challenges
    {{0x645d18, 0x37000080u, false}, Site::ClaimVisitor},    // tbnz w0, #0, 0x645d28 (w0 = the visitor test 0x376c4; kPay at 0x645d24)
    {{0x643938, 0x37001220u, false}, Site::BuyCount},        // tbnz w0, #0, 0x643b7c (w0 = the visitor test 0x376c4)
    // the creature events (pkp_coop_feed.cpp)
    {{0x1355ec, kPacibsp, true}, Site::TalkedTo},
    {{0x136548, kPacibsp, true}, Site::Fed},
    // a member's talk gift (pkp_coop_friendship.cpp)
    {{0x21c46c, 0x12000003u, false}, Site::FriendGift},      // and w3, w0, #0x1 (w0 = Cloud Island, 0x46e308's w3 at 0x21c484)
    // gimmick ownership (pkp_coop_owner.cpp): the `tbz w0, #0` after a use posts its press to the contents, five classes
    {{0x7c141c, 0x3607dec0u, false}, Site::UseRefused},      // tbz w0, #0, 0x7c0ff4 (class 0x32688e0)
    {{0x79d558, 0x36000120u, false}, Site::UseRefused},      // tbz w0, #0, 0x79d57c (class 0x3265b20)
    {{0x848eac, 0x3607ed40u, false}, Site::UseRefused},      // tbz w0, #0, 0x848c54 (class 0x327a3b8)
    {{0x79b178, 0x3607ec40u, false}, Site::UseRefused},      // tbz w0, #0, 0x79af00 (class 0x3265668)
    {{0x7e78f4, 0x36000da0u, false}, Site::UseRefused},      // tbz w0, #0, 0x7e7aa8 (class 0x326dc98)
    // a newly met creature moving in (pkp_coop_newcomer.cpp)
    {{0x48235c, 0x97fcdcc6u, false}, Site::Befriend},        // bl 0x3b9674 (x0 = the creature's component, x1 = &own user id)
    // gimmick ownership, more classes (pkp_coop_owner.cpp): the same `tbz w0, #0` after the press went to the contents
    {{0x7b9b0c, 0x36000320u, false}, Site::UseRefused},      // tbz w0, #0, 0x7b9b70 (class 0x3268000, its use)
    {{0x7b80d0, 0x36003d40u, false}, Site::UseRefused},      // tbz w0, #0, 0x7b8878 (class 0x3268000, the host's deferred use)
    {{0x82b8e4, 0x36000a80u, false}, Site::UseRefused},      // tbz w0, #0, 0x82ba34 (class 0x32762f8)
    {{0x7e469c, 0x3607e5c0u, false}, Site::UseRefused},      // tbz w0, #0, 0x7e4354 (class 0x326d7e0)
    {{0x808d68, 0x360003a0u, false}, Site::UseRefused},      // tbz w0, #0, 0x808ddc (class 0x3271340)
    {{0x83d0b0, 0x36000580u, false}, Site::UseRefused},      // tbz w0, #0, 0x83d160 (class 0x3278b08)
    {{0x85eae0, 0x36000b20u, false}, Site::UseRefused},      // tbz w0, #0, 0x85ec44 (classes 0x327cf68 / 0x327d8d8 / 0x327e248)
    // a shared object a member was using when it left (pkp_coop_gimmicks.cpp): vt+0x228 of the two multi-user classes
    {{0x74cc60, kPacibsp, true}, Site::SharedLeave},         // class 0x3262c80 (category 2)
    {{0x7568f0, kPacibsp, true}, Site::SharedLeave},         // class 0x3263160 (category 0x13)
    // a member's later talks with a resident (pkp_coop_talk.cpp)
    {{0x4c0574, 0x36000140u, false}, Site::TalkOwn},         // tbz w0, #0, 0x4c059c (w0 = 0x332c80's answer)
    // gimmick ownership, a grant that starts no use (pkp_coop_owner.cpp)
    {{0x539b44, 0x36000120u, false}, Site::GrantNoUse},      // tbz w0, #0, 0x539b68 (w0 = vt+0x2f8's answer, x23 the gimmick)
    // gimmick ownership, the host's own use of the story's materials drop ends (pkp_coop_owner.cpp)
    {{0x7e67c0, kPacibsp, true}, Site::HostUseEnd},          // class 0x326dc98's close (x0 = the gimmick)
    // the talk conditions inside a member's talk (pkp_coop_talk.cpp)
    {{0x4d35f8, 0x52000008u, false}, Site::TalkCondParent},  // eor w8, w0, #1 (w0 = IsHost's answer, in condition 0x52)
    {{0x4d0620, 0x17ed9c29u, true}, Site::TalkCondVisitor},  // b 0x376c4 (condition 0x25, all of it)
    // a gimmick's release on a member's busy request queue, the ownership log (pkp_coop_linkqueue.cpp, pkp_coop_owner.cpp)
    {{0x7b8b68, 0x36000980u, false}, Site::ReleaseGate},     // tbz w0, #0, 0x7b8c98 (class 0x3268000's close 0x7b8960)
    {{0x82a558, 0x36000920u, false}, Site::ReleaseGate},     // tbz w0, #0, 0x82a67c (class 0x32762f8's close 0x82a460)
    {{0x7e20b0, 0x36000180u, false}, Site::ReleaseGate},     // tbz w0, #0, 0x7e20e0 (0x7e2030, class 0x326d7e0)
    {{0x83bec0, 0x36000120u, false}, Site::ReleaseGate},     // tbz w0, #0, 0x83bee4 (0x83bc40, class 0x3278b08)
    {{0x83c288, 0x36000180u, false}, Site::ReleaseGate},     // tbz w0, #0, 0x83c2b8 (0x83c228, class 0x3278b08)
    {{0x83e310, 0x36000160u, false}, Site::ReleaseGate},     // tbz w0, #0, 0x83e33c (0x83e1c0)
    {{0x214974, kPacibsp, true}, Site::RequestSend},
    {{0x538f60, 0x79400008u, true}, Site::OwnerApply},       // ldrh w8, [x0]
    {{0x313eb4, 0xd63f0100u, false}, Site::OwnerPress},      // blr x8 (x0 = the object, x8 its slot 5 handler)
    {{0xa8fba0, kPacibsp, true}, Site::OwnerNotice},
    {{0x539620, kPacibsp, true}, Site::OwnerRefused},
    // a member's take-out of its Ordered Items and Gifts (pkp_coop_takeout.cpp)
    {{0x2eea74, 0x12000003u, false}, Site::TakeoutStore},    // and w3, w0, #0x1 (w0 = IsVisitor's answer, w21 = the container kind)
    // a member's deposit into the story's materials drop (pkp_coop_deposit.cpp)
    {{0x5b1c84, kPacibsp, true}, Site::DepositStore},
    {{0x7e693c, 0x36001ac0u, false}, Site::DepositConfirm},  // tbz w0, #0, 0x7e6c94 (w0 = 0xa94bbc's confirm flag, x19 the gimmick)
    // a story follow on a member (pkp_coop_follow.cpp)
    {{0x2a1ed4, 0xf0018f09u, true}, Site::StoryFollow},      // adrp x9, 0x3484000 (x0 = &the creature's actor id, w1 the flag)
    // a creature's talk record flag a member's talk arms (pkp_coop_talkrec.cpp)
    {{0x4ae7e8, 0x940b4132u, false}, Site::TalkRecArmed},    // bl 0x77ecb0 (after strb w9, [x8, #0x10] off Cloud Island; w23 = chara)
    // the Star Piece swap in a member's talk (pkp_coop_talk.cpp)
    {{0x4a1adc, 0x2a0802d8u, false}, Site::TalkSwapParent},  // orr w24, w22, w8 (w8 = the parent term, w24 = the data id, in 0x4a17fc)
    // an event's end on a member (pkp_coop_feed.cpp): the journal's entry site of the contents notifier, shared
    {{0x1353a0, kPacibsp, true}, Site::EventEnded},
    {{0x4774e8, 0x3600013cu, false}, Site::SceneTake},       // tbz w28, #0, 0x47750c (w28 = all characters ready, x19 = the runner)
    {{0x47bfe4, 0x0a000318u, false}, Site::SceneStart},      // and w24, w24, w0 (w0 = IsMine, x20 = &actor id, x19 = the runner)
};
constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t ids[kSiteCount] = {};   // all 0: not coop mode, or not installed
    CoopLaunch launch;
    uint8_t key[coop::kMemberKeyBytes] = {};   // launch.member as bytes (the hex pairs in order)
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    std::atomic<bool> in_game{false};   // play mode's, from its frame (main thread) for the game-frame tick
    std::optional<strings::Str> off_text;   // the site check turned co-op off: the notice's text, posted again once gameplay starts (frame only); its language is read at each post
};
State& st() {
    static State s;
    return s;
}

// Every co-op site (user: its Site), once the frame callback armed the hooks: the part of co-op that owns the site does its work there.
// A site no part owns yet passes through (returns 0: the game's own code runs).
int hook(::Ctx* c, uint64_t addr, void* user) {
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
        case Site::CleanCount: return coop::requests::on_clean_count(c);
        case Site::AreaMove: return coop::travel::on_area_move(c);
        case Site::GateEntry: return coop::travel::on_gate_entry(c);
        case Site::ShopGrant: return coop::purchases::on_grant(c);
        case Site::OfflineGoods: return coop::purchases::on_offline_only(c);
        case Site::ShopTile: return coop::purchases::on_shop_tile(c);
        case Site::StockGet: return coop::stock::on_get(c);
        case Site::StockSet: return coop::stock::on_set(c);
        case Site::Restock: return coop::stock::on_restock(c);
        case Site::PaneNotice: return coop::pclock::on_pane_notice(c);
        case Site::ChallengePay: return coop::money::on_pay(c);
        case Site::JoinBagSend: return coop::bag::on_join_bag_send(c);
        case Site::MirrorFill: return coop::bags::on_mirror_fill(c);
        case Site::StageBagClear: return coop::bag::on_stage_bag_clear(c);
        case Site::BagToStorage: return coop::bag::on_bag_to_storage(c);
        case Site::HostBagStore: return coop::bag::on_host_bag_store(c);
        case Site::RefusalReset: return coop::bag::on_refusal_reset(c, false);
        case Site::RefusalResetWait: return coop::bag::on_refusal_reset(c, true);
        case Site::BagScreen: return coop::bags::on_bag_screen(c);
        case Site::DexScene: return coop::discovery::on_dex_scene(c);
        case Site::HabitatScene: return coop::discovery::on_habitat_scene(c);
        case Site::VisitorSync: return coop::held::on_visitor_sync(c);
        case Site::RecordSend: return coop::linkqueue::on_send_record(c);
        case Site::QueueReset: return coop::linkqueue::on_session_reset(c);
        case Site::PlacedNotice: return coop::placement::on_placed_notice(c);
        case Site::PlayScene: return coop::contents::on_play_scene(c);
        case Site::AreaGateRecord: return coop::unlocks::on_area_gate_record(c);
        case Site::AbilityCheck: return coop::story::on_ability_check(c);
        case Site::RoleAnswer: return coop::answers::on_answer(c, addr - st().base);
        case Site::JobTick: return coop::jobs::on_job_tick(c);
        case Site::FriendGain: return coop::friendship::on_add_points(c);
        case Site::FriendFlag: return coop::friendship::on_flag_17a(c);
        case Site::FriendTag: return coop::friendship::on_name_tag(c);
        case Site::ClaimVisitor: return coop::money::on_claim_visitor(c);
        case Site::BuyCount: return coop::challenges::on_buy_visitor(c);
        case Site::TalkedTo: return coop::feed::on_talked_to(c);
        case Site::Fed: return coop::feed::on_fed(c);
        case Site::FriendGift: return coop::friendship::on_gift_apply(c);
        case Site::UseRefused: return coop::owner::on_use_refused(c, addr - st().base);
        case Site::Befriend: return coop::newcomer::on_befriend(c);
        case Site::SharedLeave: return coop::gimmicks::on_shared_leave(c, addr - st().base);
        case Site::TalkOwn: return coop::talk::on_talk_own(c);
        case Site::GrantNoUse: return coop::owner::on_grant_no_use(c);
        // the session's doors in the menus (pkp_coop_doors.cpp)
        case Site::LinkMenu: return coop::doors::on_link_menu(c);
        case Site::PauseMenu: return coop::doors::on_pause_menu(c);
        // a co-op start past the title (pkp_coop_title.cpp)
        case Site::PastTitle: return coop::title::on_past_title(c);
        case Site::HostUseEnd: return coop::owner::on_host_use_end(c);
        case Site::TalkCondParent: return coop::talk::on_cond_parent(c);
        case Site::TalkCondVisitor: return coop::talk::on_cond_visitor(c);
        case Site::ReleaseGate: return coop::linkqueue::on_release_gate(c, addr - st().base);
        case Site::RequestSend: return coop::linkqueue::on_send_request(c);
        case Site::OwnerApply: return coop::owner::on_owner_apply(c);
        case Site::OwnerPress: return coop::owner::on_press(c);
        case Site::OwnerNotice: return coop::owner::on_notice(c);
        case Site::OwnerRefused: return coop::owner::on_grant_reply(c);
        case Site::TakeoutStore: return coop::takeout::on_remove_store(c);
        case Site::DepositStore: return coop::deposit::on_store(c);
        case Site::DepositConfirm: return coop::deposit::on_confirm(c);
        case Site::StoryFollow: return coop::follow::on_story_follow(c);
        case Site::TalkRecArmed: return coop::talkrec::on_armed(c);
        case Site::TalkSwapParent: return coop::talk::on_swap_parent(c);
        case Site::EventEnded: return coop::feed::on_event_ended(c);
        case Site::SceneTake: return coop::contents::on_scene_take(c);
        case Site::SceneStart: return coop::contents::on_scene_start(c);
        // a dropped player is waited for (pkp_coop_drop.cpp)
        case Site::TripFile: return coop::drop::on_trip_file(c, addr - st().base);
        case Site::SimStep:
            coop::quality::on_sim_step(c);   // never handled: the step goes on
            return coop::drop::on_sim_step(c);
        case Site::Error3103: return coop::drop::on_error(c);
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
    auto nib = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };   // coop_launch_parse left lower-case hex digits only
    for (size_t i = 0; i < coop::kMemberKeyBytes; ++i)
        s.key[i] = static_cast<uint8_t>(nib(launch.member[2 * i]) << 4 | nib(launch.member[2 * i + 1]));
    s.base = in.base;
    coop::wirelog::install();
    coop::savecopy::install();   // before the game opens its save
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
// Co-op off at the check tells the player too (a start-up notice can be missed behind the title, so it is posted again once, at the
// first gameplay frame); otherwise they only find their own world and no host.
void frame(bool in_game, double, play::GameFrame*) {
    State& s = st();
    s.in_game.store(in_game, std::memory_order_relaxed);
    if (s.off_text && in_game) {
        notices::post(kOffNotice, strings::get(s.base, *s.off_text), kOffNoticeSeconds);
        s.off_text.reset();
    }
    if (!s.ids[0] || s.checked.exchange(true)) return;
    HookSite at[kSiteCount];
    for (size_t i = 0; i < kSiteCount; ++i) at[i] = kSites[i].at;
    uint32_t word = 0;
    if (const HookSite* bad = first_bad_site(s.base, at, kSiteCount, &word)) {
        remove_hooks(s);
        const bool relink = site_missing_from_lift(*bad, word);
        NXLOG_W(kTag, "Co-op: off: main+0x%llx is not the expected hook site (word %08x, expected %08x)%s",
                static_cast<unsigned long long>(bad->site), word, bad->word,
                relink ? " (the lift has no hook site there: Set up again)"
                       : " (this game build or dump is not the one co-op was made for)");
        s.off_text = relink ? strings::Str::CoopOffSetUpAgain : strings::Str::CoopOffVersion;
        notices::post(kOffNotice, strings::get(s.base, *s.off_text), kOffNoticeSeconds);
        if (in_game) s.off_text.reset();   // already in gameplay: this showing is the gameplay one
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
    coop::tutorials::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::hints::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::purchases::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::stock::tick(c);
    coop::shoprows::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::pclock::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::money::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::bags::tick(c);
    coop::talk::tick();
    coop::gimmicks::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::travel::tick(c);
    coop::catchup::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::linkqueue::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::notice::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::placement::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::scenes::tick(c);
    coop::contents::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::gate::tick(s.in_game.load(std::memory_order_relaxed));
    coop::trace::tick(s.in_game.load(std::memory_order_relaxed));
    coop::digest::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::records::tick(s.in_game.load(std::memory_order_relaxed));
    coop::skits::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::answers::tick();
    coop::friendship::tick(c);
    coop::feed::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::newcomer::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::takeout::tick(c);
    coop::deposit::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::follow::tick(c, s.in_game.load(std::memory_order_relaxed));
    coop::savecopy::tick(c);
    coop::talkrec::tick();
    coop::drop::tick(c, s.in_game.load(std::memory_order_relaxed));
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    s.in_game.store(false);
    s.off_text.reset();
    for (uint32_t& h : s.ids) h = 0;
    s.hooks = nullptr;
    coop::bags::reset();   // first: it writes its records' files under this player's member key
    s.launch = {};
    for (uint8_t& b : s.key) b = 0;
    coop::link::reset();
    coop::story::reset();
    coop::quests::reset();
    coop::requests::reset();
    coop::outcomes::reset();
    coop::recipes::reset();
    coop::events::reset();
    coop::discovery::reset();
    coop::unlocks::reset();
    coop::tutorials::reset();
    coop::hints::reset();
    coop::purchases::reset();
    coop::stock::reset();
    coop::shoprows::reset();
    coop::pclock::reset();
    coop::money::reset();
    coop::talk::reset();
    coop::gimmicks::reset();
    coop::travel::reset();
    coop::catchup::reset();
    coop::notice::reset();
    coop::curtain::reset();
    coop::gate::reset();
    coop::trace::reset();
    coop::digest::reset();
    coop::wirelog::reset();
    coop::held::reset();
    coop::linkqueue::reset();
    coop::placement::reset();
    coop::scenes::reset();
    coop::contents::reset();
    coop::friendship::reset();
    coop::feed::reset();
    coop::session::reset();
    coop::records::reset();
    coop::skits::reset();
    coop::answers::reset();
    coop::jobs::reset();
    coop::owner::reset();
    coop::newcomer::reset();
    coop::doors::reset();
    coop::title::reset();
    coop::takeout::reset();
    coop::deposit::reset();
    coop::follow::reset();
    coop::savecopy::reset();
    coop::talkrec::reset();
    coop::drop::reset();
    coop::quality::reset();
}

}  // namespace

namespace coop {

uint64_t base() { return st().base; }
CoopRole role() { return st().launch.role; }
int members() { return st().launch.members; }
const std::string& member_key() { return st().launch.member; }
const uint8_t* member_key_bytes() { return st().key; }

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
