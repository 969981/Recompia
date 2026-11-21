// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's messages on the co-op channel (nxrt/coop_channel.h), declared once: the channel type ids and each
// payload's byte layout. Little-endian, packed, no padding. Every payload starts with the same two bytes:
//   +0 u8 version (kVersion; a message of another version is dropped and logged once)
//   +1 u8 origin: the node of the process whose player did the thing (the authority passes a member's message on unchanged)
// Then, by type (offsets from the start of the payload; a payload shorter than its layout is dropped):
//   StoryStates +2 u16 count n, then n times: u16 quest id (1..1000), u8 that quest's state on the origin (2..4)
//   Ability     +2 u8[0x14] the ability effect command as the quest effect gave it (+0 its type, 0x01 or 0x19)
//   TalkAsk     +2 u32 character key                         (a member asks the authority for the talk lock)
//   TalkDone    +2 u32 character key                         (the holder gives the lock back)
//   TalkHolder  +2 u32 character key, +6 u8 holder node (kNobody: free)   (the authority tells every member who holds it)
//               the key is one character, not its species: (data id << 16) | idx of a resident (pkp_coop_talk.cpp)
//   NamePrompt  nothing more                                 (the talker entered their name in the game's name entry)
//   Items       +2 u16 item id, +4 u8 count (1..255)         (the talker's event script gave items)
//   StoryDigest +2 u16 count n (kQuestCount), n u8 quest states (id 1 first), then u8[kAbilityBytes] the ability bits
//                                                            (the authority's whole story, to every member every few seconds)
//   MemberName  +2 u8 length n, n bytes: the player's name as the game keeps it (UTF-8)
//   ReqUnique   +2 u8 count n, then n times: u8 unique request id (1..200), u8 its state on the origin (3..5)
//   ReqOutcome  +2 u8 unique request id (1..200)            (a member finished it here; the authority applies its world part once)
//   RecipeKnown +2 u16 count n, then n times: u16 recipe id (1..0x5db) the origin's player knows
//   WildPop     +2 u16 habitat id (1..500), +4 u16 creature data id (1..0x3ff), +6 s16 delay in seconds, +8 u8 1 add / 0 remove
//                                                            (a member's event script armed or cleared a wild creature's appearance)
//   Purchase    +2 u16 shop entry (1..0x7ff), +4 u16 count (1..999), +6 u8[16] the buyer's member key   (a member bought it)
//   PurchaseAsk +2 u64 the purchase log this member knows (0: none), +10 u32 the last entry it applied
//   PurchaseLog +2 u64 the authority's purchase log id, +10 u32 its last entry, +14 u16 count n, then n times: u32 entry number,
//               u8[16] buyer's member key, u16 shop entry, u16 count     (new purchases, or the answer to a PurchaseAsk)
//   DexKnown    +2 u16 count n, then n times: u16 Pokédex number (1..999), u8 its entry's bits on the origin (0..0xf)
//   DexScene    +2 u32 actor id, +6 u16 creature id (1..0x3ff), +8 u16 its Pokédex number   (the origin's "registered" scene)
//   HabitatKnown +2 u16 count n, then n times: u16 habitat id (1..500), u8 its bits on the origin (bit 0 found, bit 1 scene shown)
//   HabitatScene +2 u16 habitat id (1..500), +4 u16 placed habitat (0..0x3ff), +6 u8 flag (0..1)   (the origin's "new habitat" scene)
//   EventHistory +2 u16 n, n bytes: the origin's played-scene bits (byte i bit j = scene id i * 8 + j, ids 1..2998), then u16 m,
//                m bytes: its conversation rows (row 0..m-1, bit 0 kept)   (whole, when either grew; a receiver takes the union)
//   StoryLines   +2 u8 count n, then n times: u8 story line (1..0xfe), u8 the line's id, s16 its step, u16 its condition bits
//                                                            (the lines that changed on the origin; a receiver takes the furthest)
//   QuestFlagAsk +2 u16 quest id (1..1000), +4 u8 its state on the origin (1..3)   (a member's player made that state's next step
//               happen: the authority sets the quest's flag when its own state is the same; pkp_coop_quests.cpp)
//   UnlockKnown +2 u8 list, +3 u16 count n, then n times: u8 entry id the origin's player has   (pkp_coop_unlocks.cpp)
//               lists: 1 moves (id 1..0x3c), 2 emotes (1..0x64), 3 areas visited (stage 1..5), 4 buildings built (0..0x7f);
//               a list this build does not know is skipped
//   AbilityCleared +2 u16 count n, then n u8 ability ids (1..0xf9) a step took away on the origin (pkp_coop_story.cpp): every
//               receiver clears them and never gives them back in this co-op run
//   AreaMusic   +2 u8 the authority's area (stage), +3 u8 that area's quest music there (m_contentsBGM; 0: none), +4 u8 the play flag
//               of the quest step that set it (1 when not known)   (with every digest; pkp_coop_story.cpp)
//   ChallengesClaimed +2 u16 count n, then n u16 challenge ids (1..0x4b0) claimed in the authority's save (pkp_coop_money.cpp): a
//               member on an older build pays itself their coins once; a newer member ignores it (each claims its own at the PC)
//   StateDigest +2 u8 count n, then n times: u8 domain, u32 hash of the domain's entries, u16 how many entries are set
//               (the authority's shared state, every kDigestEvery seconds; pkp_coop_digest.cpp: a member compares its own)
//   StateAsk    +2 u8 domain                               (a member whose digest differed twice asks for the authority's entries)
//   StateFull   +2 u8 domain, +3 u16 count n, then n times: u32 entry id, u32 its value   (the authority's set entries)
//   StateReport +2 u8 domain, +3 u16 differing n, +5 u16 count m (m <= n), then m times: u32 entry id, u32 the authority's value,
//               u32 the member's value                     (a member's differences, so the host's log shows them too)
//               These four are logging only: a build without them drops them and plays on (no kVersion, no co-op rules change).
//   BagRecord   +2 u8[16] the member key the bag belongs to, +18 u64 its record's sequence (0: this process has no record), +26
//               u8[16] the player's user id as the game keeps it (its row in the host's member table), +42 kBagRecordSlots x 16
//               bytes, the bag's slots (pkp_coop_bags.cpp; each slot's first 8 bytes are 0, its last 8 the game's payload)
//               (a member's own record, to the authority: at session up, then at each change)
//   BagRestore  +2 u8[16] member key, +18 u8 1 when the authority has a record for it (0: none, the rest is 0), +19 u64 sequence,
//               +27 kBagRecordSlots x 16 bytes   (the authority's answer to a member's first BagRecord of a session, or to one older than
//               its own copy; a member ignores one for another member's key)
//   StoryRecord +2 u8 record (1 effect commands, 2 town flags, 3 town targets, 4 stage flags, 5 hint groups, 6 the materials drop;
//               another is skipped),
//               +3 u8 the authority's area (stage), +4 u16 length n (the record's size; another is skipped), n bytes: the record as the
//               authority's save holds it   (when it changed, and all of them on a join and every so often; pkp_coop_records.cpp)
//   FriendAsk   +2 u8 kind, +3 u16 the creature's actor handle (kind 1; 0 for 2), +5 u32 its chara (< 0x41a), +9 s16 amount (kind 1,
//               >= 1; 0 for 2), +11 u16 a record byte (kind 1: the caller's once-gate, 0 none / 0x175 / 0x177 / 0x17d; kind 2: 0x17a)
//               (kind 1: a member's friendship gain with that creature; kind 2: a member set that friend record flag;
//               pkp_coop_friendship.cpp)
//   FriendState +2 u16 count n, then n times: u16 chara (< 0x41a), u16 friendship points (+0x2c), u16 meter (+0x2e), u8 the bytes
//               +0x175, +0x177, +0x17a, +0x17d   (the authority's friend records that changed, or all set ones to a member that joins)
//   CreatureEvent +2 u8 event kind (1: talked to, 29: fed), +3 u8 the origin's stage, +4 u16 the creature's data id (1..0x3ff), +6 u16
//               the kind's argument (kind 1: 0; kind 29: bits 0..12 the item, bit 15 the feed's flag)   (pkp_coop_feed.cpp: a
//               member's player did it; the authority makes it again for its own creature; a kind it does not post is dropped)
//   CreatureEvent +2 u8 event kind (1: talked to), +3 u8 the origin's stage, +4 u16 the creature's data id (1..0x3ff), +6 u16 the
//               kind's argument (kind 1: 0)   (pkp_coop_feed.cpp: a member's player did it; the authority posts it to its contents;
//               a kind the authority does not post is dropped)
//   NewcomerAsk +2 u16 the creature's actor handle, +4 u16 its chara (< 0x41a), +6 u8 the origin's stage   (a member's first talk
//               with a creature no player had befriended; the authority makes the host's befriend for it; pkp_coop_newcomer.cpp)
//   DepositAsk  +2 u8[16] the asker's member key, +18 u64 ask id (the same on every resend), +26 u32 the drop's cell (x | y << 16),
//               +30 u16 its z, +32 u8 the stage the asker's drop is at, +33 u8 count n (0..3), then n times: u16 item, u8 count
//               (a member confirmed a deposit into the story's materials drop; the authority steps the drop once when its own drop
//               is at that stage and the items and the group's conditions meet the stage's row; pkp_coop_deposit.cpp)
//   DepositState +2 u32 the drop's cell (x | y << 16), +6 u16 its z, +8 u8 the authority's drop stage, +9 u8 verdict (0 none: the
//               stage only, to every member; 1 accepted, 2 refused: the answer to one ask), +10 u8 reason (refused: 1 the stage
//               differs, 2 the stage's needs are not met, 3 no drop at the cell), +11 u8[16] the asker's member key (0 for none),
//               +27 u64 its ask id (0 for none)
//   StoryFollow +2 u8 the origin's stage, +3 u16 the creature's data id (1..0x3ff), +5 u8 the attach's flag   (a member's story
//               content asked the creature to follow its player; the authority attaches its own copy to its own player unless it
//               follows someone already; pkp_coop_follow.cpp)
// The shared save (pkp_coop_savecopy.cpp, pkp_coop_saveset.h). A file list is u8 count n (<= 32), then n times: u8 name length L
// (1..40), L bytes of the name relative to the account folder ("G00/STGDAT01.BIN"; only world files), u32 size, u64 hash
// (saveset::hash), and in a SaveOffer one more u8: 1 when its bytes follow in SaveChunks to this member.
//   SaveHave    +2 u8[16] the member key, +18 u64 its copy's seq, +26 u64 its saved_at, +34 u8 status (0 hello, 1 applied the offered
//               copy, 2 a copy failed its check), +35 a file list: its copy's world files   (a member to the authority)
//   SaveOffer   +2 u64 seq, +10 u64 saved_at, +18 u8[16] the authority's member key (the copy's writer), +34 a file list (with the sent
//               byte), then u8 count m, m times: u8[16] member key, u64 seq, u64 saved_at (every copy the authority knows, its own too)
//   SaveChunk   +2 u64 the copy's seq, +10 u8 file index (in the offer's list), +11 u32 offset, +15 u32 length n, +19 n bytes of the
//               file   (in order, after the offer; a member writes them into incoming/)
//   TalkRecArmed +2 u16 the creature's chara (< 0x41a)   (a member's talk set that creature's talk record flag +0x10; the authority
//               sets it in its own record; pkp_coop_talkrec.cpp)
//   ---- a dropped player is waited for (pkp_coop_drop.cpp) ----
//   DropHere    +2 u8[16] the member's key, +18 u8 1 once its player is in the world (0: in the session, still arriving)
//               (a member, at each session join; the authority waits only for members that said it)
//   DropLeaving nothing more                                 (a member's own leave is filed: its leave that follows is no drop)
//   DropWait    +2 u8 count n, then n times: u8[16] member key, u8 state (1 awaited, 2 joining again, 3 back, 4 no longer awaited), u16
//               seconds left on the authority's clock, u8 name length m, m bytes the player's name (UTF-8)   (the authority, every
//               second while anyone is awaited and at each change; count 0: no one is)
//   DropClosing nothing more                                 (the authority's own Save and Quit closes the session for everyone)
// A change to a layout raises kVersion. A new type does not: a build without its handler drops it at drain (one warning per type,
// nxrt coop_channel drain()), and the rest plays on. But a dropped message is never handled, so it is never forwarded either: with
// an older build as the authority, a new shared type reaches no member at all (the authority is the only one that passes shared
// messages on); with an older build as a member, only that member misses it. A new type that the session cannot play without (a
// member relies on the authority handling it, as QuestFlagAsk) raises the co-op rules instead (pkp_coop.h kCoopRules): those are
// in the session's mods fingerprint, so builds with other rules are refused at the join.
// Every payload's origin is checked against its real sender (pkp_coop_link.cpp deliver()): a member's message comes from its own
// node, a shared message a member receives comes from the authority, and the authority-only types come from the authority alone.
//   StoryStates states: 2 started, 3 complete, 4 closed; any other value is dropped. StoryDigest states: 0..4, a higher one is
//   skipped for that quest. Items counts: 1..255 per message; a larger give is sent as several messages.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nxrt::games::pkp::coop::wire {

constexpr uint8_t kVersion = 3;   // 2: StoryStates replaced 1's quest flag sets; 3: StoryDigest, MemberName
constexpr uint8_t kNobody = 0xff;
constexpr size_t kHeader = 2;
constexpr size_t kEffectSize = 0x14;
constexpr size_t kAbilityBytes = 0x20;   // ability ids 0..0xff, one bit each
constexpr size_t kNameMax = 0x40;
constexpr size_t kBagRecordSlots = 130;  // a whole bag as the game copies one (layout::kBagWholeSlots)
constexpr size_t kBagSlotBytes = 16;

enum class Type : uint32_t {
    StoryStates = 1,   // u16 count, then per quest: u16 id, u8 state
    Ability = 2,
    TalkAsk = 3,
    TalkDone = 4,
    TalkHolder = 5,
    NamePrompt = 6,
    Items = 7,
    StoryDigest = 8,
    MemberName = 9,
    ReqUnique = 10,
    ReqOutcome = 11,
    RecipeKnown = 12,
    WildPop = 13,
    Purchase = 14,
    PurchaseAsk = 15,
    PurchaseLog = 16,
    DexKnown = 17,
    DexScene = 18,
    HabitatKnown = 19,
    HabitatScene = 20,
    EventHistory = 21,
    StoryLines = 22,
    QuestFlagAsk = 24,   // with the story's types (pkp_coop_quests.cpp)
    UnlockKnown = 23,
    AbilityCleared = 25,
    AreaMusic = 26,
    ChallengesClaimed = 27,
    StateDigest = 28,   // the shared-state digests (pkp_coop_digest.cpp), logging only
    StateAsk = 29,
    StateFull = 30,
    StateReport = 31,
    BagRecord = 32,
    BagRestore = 33,
    StoryRecord = 34,   // the story records (pkp_coop_records.cpp)
    // ---- friendship (pkp_coop_friendship.cpp) ----
    FriendAsk = 40,
    FriendState = 41,
    // the creature events (pkp_coop_feed.cpp); 37 is kept for its next kind of message
    CreatureEvent = 36,
    // the group travel (pkp_coop_travel.cpp): u8 the member's player index in the session, u8 the stage
    TravelAsk = 42,
    // ---- a newly met creature moving in (pkp_coop_newcomer.cpp) ----
    NewcomerAsk = 38,
    // ---- a member's deposit into the story's materials drop (pkp_coop_deposit.cpp) ----
    DepositAsk = 44,
    DepositState = 45,
    // ---- a story follow on a member (pkp_coop_follow.cpp) ----
    StoryFollow = 39,
    // ---- the shared save (pkp_coop_savecopy.cpp); 51 is kept for its next kind of message ----
    SaveHave = 48,
    SaveOffer = 49,
    SaveChunk = 50,
    // ---- a creature's talk record flag a member's talk arms (pkp_coop_talkrec.cpp) ----
    TalkRecArmed = 46,
    // ---- a dropped player is waited for (pkp_coop_drop.cpp) ----
    DropHere = 52,
    DropLeaving = 53,
    DropWait = 54,
    DropClosing = 55,
};

// Builds one payload: the header, then the fields in order.
class Writer {
public:
    explicit Writer(uint8_t origin) : b_{kVersion, origin} {}
    Writer& u8(uint8_t v) {
        b_.push_back(v);
        return *this;
    }
    Writer& u16(uint16_t v) { return u8(static_cast<uint8_t>(v)).u8(static_cast<uint8_t>(v >> 8)); }
    Writer& u32(uint32_t v) { return u16(static_cast<uint16_t>(v)).u16(static_cast<uint16_t>(v >> 16)); }
    Writer& u64(uint64_t v) { return u32(static_cast<uint32_t>(v)).u32(static_cast<uint32_t>(v >> 32)); }
    Writer& bytes(const uint8_t* p, size_t n) {
        b_.insert(b_.end(), p, p + n);
        return *this;
    }
    std::vector<uint8_t> take() { return std::move(b_); }

private:
    std::vector<uint8_t> b_;
};

// Reads one payload: header() first, then the fields in order; ok() is false once a read ran past the end or the version differs.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool header(uint8_t* origin) {
        const uint8_t version = u8();
        *origin = u8();
        if (version != kVersion) ok_ = false;
        return ok_;
    }
    uint8_t u8() {
        if (at_ + 1 > n_) {
            ok_ = false;
            return 0;
        }
        return p_[at_++];
    }
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | u8() << 8);
    }
    uint32_t u32() {
        const uint32_t lo = u16();
        return lo | static_cast<uint32_t>(u16()) << 16;
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | static_cast<uint64_t>(u32()) << 32;
    }
    void bytes(uint8_t* out, size_t n) {
        if (at_ + n > n_) {
            ok_ = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, p_ + at_, n);
        at_ += n;
    }
    bool ok() const { return ok_; }

private:
    const uint8_t* p_;
    size_t n_, at_ = 0;
    bool ok_ = true;
};

}  // namespace nxrt::games::pkp::coop::wire
