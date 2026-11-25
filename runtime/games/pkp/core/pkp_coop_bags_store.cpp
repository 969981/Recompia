// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's bag records, their files (pkp_coop_bags_internal.h; the account of the records is the header of
// pkp_coop_bags.cpp): a file per member key, <save dir>/bags/<member key>.bin, "CBAG", u8 format, u64 sequence, 130 x 16 bytes
// (the first 8 of each 0), written whole (temp file + rename) at most every kWriteEvery while it changed, and at session end.
#include "core/pkp_coop_bags_internal.h"
#include "core/pkp_save_files.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "nxrt/file_write.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::bags {

namespace {

constexpr const char* kTag = "pkp";

constexpr double kWriteEvery = 2.0;   // seconds between two writes of one record's file

constexpr char kFileMagic[4] = {'C', 'B', 'A', 'G'};
constexpr uint8_t kFormat = 1;
constexpr size_t kFileHead = sizeof kFileMagic + 1 + 8;

std::filesystem::path file_of(const Key& k) { return save_files::save_dir() / "bags" / (hex(k) + ".bin"); }

bool read_file(const Key& k, Record* out) {
    std::string b;
    if (std::ifstream in{file_of(k), std::ios::binary}) b.assign(std::istreambuf_iterator<char>(in), {});
    if (b.empty()) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(b.data());
    if (b.size() != kFileHead + kBagBytes || std::memcmp(p, kFileMagic, sizeof kFileMagic) != 0 || p[4] != kFormat) {
        NXLOG_W(kTag, "Co-op bags: %s is not a bag record of this build; it is not used", save_files::u8(file_of(k)).c_str());
        return false;
    }
    std::memcpy(&out->seq, p + 5, 8);
    for (size_t i = 0; i < kSlots; ++i) {
        std::memcpy(&out->slot[i], p + kFileHead + i * items::kSlotSize + items::kSlotPayload, 8);
        out->slot[i] = items::moved_of(out->slot[i]);
    }
    return true;
}

void write_file(const Key& k, Kept& kept, double now) {
    std::string b(kFileHead + kBagBytes, '\0');
    std::memcpy(b.data(), kFileMagic, sizeof kFileMagic);
    b[4] = static_cast<char>(kFormat);
    std::memcpy(b.data() + 5, &kept.rec.seq, 8);
    for (size_t i = 0; i < kSlots; ++i) std::memcpy(b.data() + kFileHead + i * items::kSlotSize + items::kSlotPayload, &kept.rec.slot[i], 8);
    std::string err;
    kept.written_at = now;
    if (!write_file_atomic(save_files::u8(file_of(k)), b, &err)) {
        NXLOG_W(kTag, "Co-op bags: the record of %s is not written: %s", hex(k).c_str(), err.c_str());
        return;
    }
    kept.dirty = false;
    const auto [slots, count] = totals(kept.rec);
    NXLOG_I(kTag, "Co-op bags: kept %s record %llu (%u slots, %u items)", k == own_key() ? "this player's" : hex(k).c_str(),
            static_cast<unsigned long long>(kept.rec.seq), slots, count);
}

}  // namespace

// Writes every changed record whose last write is kWriteEvery old (all of them with force).
void flush(bool force) {
    State& s = st();
    const double now = qpc::seconds();
    if (s.own.dirty && (force || now - s.own.written_at >= kWriteEvery)) write_file(own_key(), s.own, now);
    for (auto& [k, kept] : s.members)
        if (kept.dirty && (force || now - kept.written_at >= kWriteEvery)) write_file(k, kept, now);
}

Kept& own() {
    State& s = st();
    if (!s.loaded) {
        s.loaded = true;
        if (read_file(own_key(), &s.own.rec))
            NXLOG_I(kTag, "Co-op bags: this player's record %llu read", static_cast<unsigned long long>(s.own.rec.seq));
    }
    return s.own;
}

Kept& member(const Key& k) {
    auto [it, added] = st().members.try_emplace(k);
    if (added) read_file(k, &it->second.rec);
    return it->second;
}

}  // namespace nxrt::games::pkp::coop::bags
