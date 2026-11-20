// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's memory-only actor and data-table lookups (pkp_journal_actor.h). Read from main.img (2.0.1a):
//   0x281360(actors, &u16 id)  type = id >> 12: 2 -> u64 [actors + idx*8 + 0x50]; 3 -> idx <= 1, [+ idx*8 + 0x250]; 4 -> idx <= 4,
//                              [+ idx*8 + 0x260]; 1 -> the player among actors +0x30..+0x48 whose net slot (vt+0x4a8) is (id - 1) & 0xff
//   0x285434(&u16 id)          the species: the actor, vt+0x1d8 true -> vt+0x410 part u16 +0x39f0; else vt+0x1f0 true -> vt+0x420 part
//                              u16 +0x2ba4; that data id (1..0x3ff) is a row of the table at +0x270 (held while the count > 0x4e), record
//                              (or the default [main+0x3483b40]) u16 +0x12
//   0xae9d9c (table vt+0x38)   `ldr x8, [x0, #0x10]; cmp w1, #0; csel x0, x8, xzr, eq; ret`: the rows at table +0x10 for index 0
//   0xa343b4 (rows vt+0x30)    id >= 0 and u64 rows+0x10 and u64 rows+8 set and id < u32 [rows+8]+0x30 -> u64 [[rows+0x10] + id*8]
#include "core/pkp_journal_actor.h"

#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::journal::actor {

namespace {

constexpr uint64_t kActorsGot = 0x3483bd0;                 // main + : -> the actor manager (= the player manager)
constexpr uint64_t kVtNetSlot = 0x4a8;                     // a player's net slot getter (0x66725c: u8 +0x5a38)
constexpr uint64_t kVtIsResident = 0x1d8, kVtResident = 0x410, kVtIsOther = 0x1f0, kVtOther = 0x420;
constexpr uint64_t kDataId = 0x39f0, kIdx = 0x39f2, kTalkKey = 0x398e, kOtherDataId = 0x2ba4;
constexpr uint32_t kDataIds = 0x3ff, kOtherKey = 0x80000000u;
constexpr uint64_t kCharaTable = 0x270, kCharaDefaultGot = 0x3483b40, kSpecies = 0x12;
constexpr uint64_t kRowsGetter = 0xae9d9c, kRowGetter = 0xa343b4;   // the two table getters this reads (anything else: unread)

}  // namespace

std::optional<uint64_t> leaf(uint64_t obj, uint64_t slot) {
    constexpr uint32_t kRet = 0xd65f03c0u;
    const uint64_t vt = guest::deref(obj);
    const uint64_t fn = vt ? guest::deref(vt + slot) : 0;
    uint32_t w[3] = {};
    if (!fn || !guest::read(fn, w, sizeof w)) return std::nullopt;
    if (w[0] == kRet) return obj;
    if ((w[0] & 0xffe0001fu) == 0x52800000u && w[1] == kRet) return (w[0] >> 5) & 0xffff;
    if (w[0] == 0x2a1f03e0u && w[1] == kRet) return 0;
    uint8_t b = 0;
    if ((w[0] & 0xffe0001fu) == 0x52800008u && w[1] == 0x38686800u && w[2] == kRet) {
        if (!guest::read(obj + ((w[0] >> 5) & 0xffff), b)) return std::nullopt;
        return b;
    }
    if ((w[0] & 0xffc003ffu) == 0x39400000u && w[1] == kRet) {
        if (!guest::read(obj + ((w[0] >> 10) & 0xfff), b)) return std::nullopt;
        return b;
    }
    return std::nullopt;
}

uint64_t actors() { return guest::deref(guest::deref(base() + kActorsGot)); }

std::optional<uint64_t> find(uint64_t pm, uint16_t id) {
    const uint32_t type = id >> 12;
    if (type - 1 > 3) return 0;
    const uint32_t idx = id & 0xfff;
    if (type == 2) return guest::deref(pm + idx * 8 + 0x50);
    if (type == 3) return idx > 1 ? 0 : guest::deref(pm + idx * 8 + 0x250);
    if (type == 4) return idx > 4 ? 0 : guest::deref(pm + idx * 8 + 0x260);
    if ((id & 0xff) > 4) return 0;
    for (uint64_t off = 0x30; off <= 0x48; off += 8) {
        const uint64_t p = guest::deref(pm + off);
        if (!p) continue;
        const auto net = leaf(p, kVtNetSlot);
        if (!net) return std::nullopt;
        if ((*net & 0xff) == ((id - 1u) & 0xff)) return p;
    }
    return 0;
}

std::optional<uint64_t> table_row(uint64_t off, uint32_t id) {
    const uint64_t mgr = guest::deref(base() + layout::kTables);
    uint32_t count = 0;
    if (!mgr || !guest::read(mgr + layout::kTableCount, count)) return std::nullopt;
    const uint64_t table = guest::deref(mgr + 0x10 + (count > off / 8 ? off : 0));
    if (!table) return 0;
    const uint64_t vt = guest::deref(table);
    if (!vt || guest::deref(vt + 0x38) != base() + kRowsGetter) return std::nullopt;
    const uint64_t rows = guest::deref(table + 0x10);
    if (!rows) return 0;
    const uint64_t rvt = guest::deref(rows);
    if (!rvt || guest::deref(rvt + 0x30) != base() + kRowGetter) return std::nullopt;
    const uint64_t arr = guest::deref(rows + 0x10), info = guest::deref(rows + 8);
    uint32_t n = 0;
    if (id >> 31 || !arr || !info || !guest::read(info + 0x30, n) || n <= id) return 0;
    return guest::deref(arr + uint64_t{id} * 8);
}

uint64_t record(uint64_t entry, uint64_t default_got) {
    const uint64_t r = entry ? guest::deref(entry + 8) : 0;
    return r ? r : guest::deref(base() + default_got);
}

Character character(uint16_t id) {
    Character ch;
    ch.id = id;
    const uint64_t pm = actors();
    const auto a = pm ? find(pm, id) : std::optional<uint64_t>{};
    if (!a) return ch;
    ch.actor = *a;
    if (!ch.actor) {
        ch.read = true;
        return ch;
    }
    const auto is_res = leaf(ch.actor, kVtIsResident);
    if (!is_res) return ch;
    uint64_t data_at = 0;
    if (*is_res & 1) {
        const auto part = leaf(ch.actor, kVtResident);
        if (!part) return ch;
        ch.resident = true;
        ch.read = true;
        if (*part) {
            uint8_t tk = 0;
            if (guest::read(*part + kTalkKey, tk)) ch.talk_key = tk;
            guest::read(*part + kDataId, ch.data_id);
            guest::read(*part + kIdx, ch.idx);
            data_at = *part;
            if (ch.data_id - 1u < kDataIds) ch.key = (uint32_t{ch.data_id} << 16) | ch.idx;
        }
    }
    if (!data_at) {   // 0x285434 reads the other target only when vt+0x1d8 is false or the resident part is null
        const auto is_other = leaf(ch.actor, kVtIsOther);
        if (!is_other) return ch;
        ch.read = true;
        if (!(*is_other & 1)) return ch;
        const auto part = leaf(ch.actor, kVtOther);
        if (!part) {
            ch.read = false;
            return ch;
        }
        ch.other = true;
        uint16_t d = 0;
        if (*part && guest::read(*part + kOtherDataId, d) && d - 1u < kDataIds) {
            ch.data_id = d;
            ch.key = kOtherKey | (uint32_t{d} << 16);
            data_at = *part;
        }
    }
    if (data_at && ch.data_id - 1u < kDataIds) {
        const auto row = table_row(kCharaTable, ch.data_id);
        uint16_t sp = 0;
        if (row && guest::read(record(*row, kCharaDefaultGot) + kSpecies, sp)) ch.species = sp;
    }
    return ch;
}

std::string describe(const Character& ch) {
    if (!ch.read) return fmt("actor=0x%x (not read: a getter is not a leaf)", ch.id);
    if (!ch.actor) return fmt("actor=0x%x (none)", ch.id);
    std::string s = fmt("actor=0x%x %s", ch.id, ch.resident ? "resident" : ch.other ? "other" : "no talk data");
    if (ch.data_id) s += fmt(" data id %u idx %u key=0x%08x", ch.data_id, ch.idx, ch.key);
    s += ch.species >= 0 ? fmt(" species %d", ch.species) : std::string(" species ?");
    if (ch.resident) s += ch.talk_key >= 0 ? fmt(" talk-key %d", ch.talk_key) : std::string(" talk-key ?");
    return s;
}

}  // namespace nxrt::games::pkp::journal::actor
