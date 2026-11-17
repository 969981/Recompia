// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op's messages on the co-op channel (nxrt/coop_channel.h), declared once: the channel type ids and each
// payload's byte layout. Little-endian, packed, no padding. Every payload starts with the same two bytes:
//   +0 u8 version (kVersion; a message of another version is dropped and logged once)
//   +1 u8 origin: the node of the process whose player did the thing (the authority passes a member's message on unchanged)
// Then, by type (offsets from the start of the payload; a payload shorter than its layout is dropped):
//   StoryStates +2 u16 count n, then n times: u16 quest id (1..1000), u8 that quest's state on the origin (2..4)
//   Ability     +2 u8[0x14] the ability effect command as the quest effect gave it (+0 its type, 0x01 or 0x19)
//   TalkAsk     +2 u32 character id                          (a member asks the authority for the talk lock)
//   TalkDone    +2 u32 character id                          (the holder gives the lock back)
//   TalkHolder  +2 u32 character id, +6 u8 holder node (kNobody: free)   (the authority tells every member who holds it)
//   NamePrompt  nothing more                                 (the talker entered their name in the game's name entry)
//   Items       +2 u16 item id, +4 u8 count (1..255)         (the talker's event script gave items)
//   StoryDigest +2 u16 count n (kQuestCount), n u8 quest states (id 1 first), then u8[kAbilityBytes] the ability bits
//                                                            (the authority's whole story, to every member every few seconds)
//   MemberName  +2 u8 length n, n bytes: the player's name as the game keeps it (UTF-8)
//   ReqUnique   +2 u8 count n, then n times: u8 unique request id (1..200), u8 its state on the origin (3..5)
// A change to a layout raises kVersion. A new type does not: a build without its handler drops it at drain (one warning per type,
// nxrt coop_channel drain()), and the rest plays on. But a dropped message is never handled, so it is never forwarded either: with
// an older build as the authority, a new shared type reaches no member at all (the authority is the only one that passes shared
// messages on); with an older build as a member, only that member misses it.
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
