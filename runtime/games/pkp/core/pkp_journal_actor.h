// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal watchers' memory-only versions of the game's actor and data-table lookups (pkp_journal_watch.h: watchers
// never call into the game). Each one reads what the game's own function reads, in its order, and answers "unread" (nullopt) when a
// getter it would call is not one of the leaf shapes it can read, so a watcher prints raw values or "?" instead of guessing.
//   find()       0x281360(actors, &u16 id): the actor with that id (actors = [[main+0x3483bd0]] = the player manager)
//   character()  the talk target's identity as 0x285434 and co-op's lock key read it (resident vt+0x1d8 / +0x410, other vt+0x1f0 /
//                +0x420), plus its species and its talk key byte
//   table_row()  the data-table manager's row lookup (table vt+0x38 = 0xae9d9c, rows vt+0x30 = 0xa343b4, both leaves)
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace nxrt::games::pkp::journal::actor {

// A virtual getter's answer for obj (vtable slot `slot`), read from its code when it is a leaf shape: `ret` (obj itself),
// `mov w0, #imm; ret`, `mov w0, wzr; ret`, `mov w8, #imm; ldrb w0, [x0, x8]; ret`, `ldrb w0, [x0, #imm]; ret`. nullopt otherwise.
std::optional<uint64_t> leaf(uint64_t obj, uint64_t slot);

// The actor manager [[main+0x3483bd0]] (0x281360's x0), 0 before it is made.
uint64_t actors();

// 0x281360(actors, &id) by reads: the actor (0 = none), or nullopt when a net-slot getter (vt+0x4a8) is not a leaf.
std::optional<uint64_t> find(uint64_t actors, uint16_t id);

// The row entry of the data-table manager's table at `off` (the table at +0 while the manager does not hold `off`, as the game reads
// it), 0 when there is no such row, nullopt when the manager or a getter is not what this reads.
std::optional<uint64_t> table_row(uint64_t off, uint32_t id);
// A row entry's record: entry+8, or the table's default record ([main + default_got]) when that is 0.
uint64_t record(uint64_t entry, uint64_t default_got);

// A talk target, read once.
struct Character {
    uint16_t id = 0;          // the u16 actor id
    uint64_t actor = 0;       // 0: none with that id
    bool read = false;        // false: a getter on the way was not a leaf; the fields below are not known
    bool resident = false;    // vt+0x1d8 true: a resident (its part vt+0x410: +0x39f0 data id, +0x39f2 idx, +0x398e talk key)
    bool other = false;       // vt+0x1f0 true: another talk target (its part vt+0x420: +0x2ba4 data id)
    uint16_t data_id = 0, idx = 0;
    uint32_t key = 0;         // co-op's lock key: (data id << 16) | idx, or bit 31 | (data id << 16) for an other target
    int species = -1;         // u16 +0x12 of the data id's row in the table at +0x270 (0x285434), -1 unread
    int talk_key = -1;        // u8 +0x398e of the resident part (0x4cc1a8 / 0x4cd518 match condition rows on it), -1 unread
};
Character character(uint16_t id);
// "actor=0x2031 resident data id 352 idx 2 key=0x01600002 species 375 talk-key 3" (raw values; "?" where unread).
std::string describe(const Character& ch);

}  // namespace nxrt::games::pkp::journal::actor
