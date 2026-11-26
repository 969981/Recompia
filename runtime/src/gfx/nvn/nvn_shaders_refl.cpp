// SPDX-License-Identifier: GPL-3.0-or-later
// The shader cache's reflection catalogue (ShaderCache::cat_*, nvn_shaders.h): reflect_spirv's results for the preload pack's entries,
// kept in <dir>.preload.pack.refl beside the pack so a launch does not reflect the pack's SPIR-V again.
#include "nvn_shaders.h"
#include "nvn_shaders_internal.h"

#include <windows.h>

#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <nxrt/config.h>
#include <nxrt/log.h>

namespace nxrt::nvn::be {

using namespace shader_files;

// ---- the reflection catalogue ---------------------------------------------------------------------------------------
// <dir>.preload.pack.refl, beside the pack: reflect_spirv's result (before the resources record is applied) for the entries of one
// pack generation. Reflecting a pack of thousands of shaders walks all of their words (7.1 s of one core on a Snapdragon 8 Gen 3, after
// decompressing them); far fewer distinct records answer the same questions. Layout, little endian:
//   CatHeader, u32 index[pack_count] (pack slot -> record, ~0 none), u32 offs[records + 1] (record bytes, from the records' start),
//   the records, u64 FNV-1a 64 of everything before it.
// Valid only for the pack whose index digest, size and count it names, written by this schema: anything else, a short file, a bad
// checksum or a record that does not decode is ignored and the parser runs (and the catalogue is written again after the preload).
// A pack entry translated again since open() (pack_stale) is read from elsewhere and reflected by the parser, as before.
// The first run with a new pack reflects as before and writes the catalogue when its preload is done (records of shaders the preload
// did not reach are added by later runs). [nvn] refl_catalogue = 0: never read or written. [nvn] refl_catalogue_verify = 1: every
// catalogue answer is also reflected by the parser and compared (a diagnostic; mismatches are logged and the parser's answer used).
// Fields added to ShaderReflection, DescriptorBinding or ShaderIo must be added to CatCodec and kCatSchema raised.
namespace {
constexpr char kCatMagic[8] = {'N', 'X', 'S', 'R', 'E', 'F', 'L', '1'};
constexpr uint32_t kCatSchema = 1;
struct CatHeader {
    char magic[8];
    uint32_t schema;
    uint32_t pack_count;
    uint64_t pack_digest;
    uint64_t pack_size;
    uint32_t records;
    uint32_t record_bytes;
};
static_assert(sizeof(CatHeader) == 40, "catalogue header layout");
static_assert(std::endian::native == std::endian::little, "the catalogue is written little endian");

bool cat_enabled() {
    static const bool on = nxrt::config_get_bool("nvn", "refl_catalogue", true);
    return on;
}
bool cat_verify() {
    static const bool on = nxrt::config_get_bool("nvn", "refl_catalogue_verify", false);
    return on;
}

// Explicit widths; a reader checks every length and enum and fails rather than guess.
struct CatCodec {
    std::string* out = nullptr;            // writing
    const uint8_t* p = nullptr;            // reading
    const uint8_t* end = nullptr;
    bool ok = true;
    void raw(void* v, size_t n) {
        if (out) {
            out->append(static_cast<const char*>(v), n);
        } else if (ok && static_cast<size_t>(end - p) >= n) {
            std::memcpy(v, p, n);
            p += n;
        } else {
            ok = false;
        }
    }
    void u32(uint32_t& v) { raw(&v, 4); }
    void i32(int& v) { int32_t x = v; raw(&x, 4); v = x; }
    void u8(uint8_t& v) { raw(&v, 1); }
    void b(bool& v) { uint8_t x = v; raw(&x, 1); ok = ok && x <= 1; v = x != 0; }
    template <class E> void e8(E& v, unsigned max) { uint8_t x = static_cast<uint8_t>(v); raw(&x, 1); ok = ok && x <= max; v = static_cast<E>(x); }
    void str(std::string& s) {
        uint32_t n = static_cast<uint32_t>(s.size());
        u32(n);
        if (out) { out->append(s); return; }
        if (!ok || static_cast<size_t>(end - p) < n) { ok = false; return; }
        s.assign(reinterpret_cast<const char*>(p), n);
        p += n;
    }
    template <class T, class F> void vec(std::vector<T>& v, F&& each) {
        uint32_t n = static_cast<uint32_t>(v.size());
        u32(n);
        if (!out) {
            if (!ok || n > static_cast<size_t>(end - p)) { ok = false; return; }   // every element takes at least one byte
            v.resize(n);
        }
        for (T& x : v) each(x);
    }
    void io(ShaderIo& v) { u32(v.location); u32(v.component); e8(v.type, 4); u32(v.components); u32(v.array); }
    void binding(DescriptorBinding& v) {
        u32(v.set); u32(v.binding);
        uint32_t t = static_cast<uint32_t>(v.type); u32(t); ok = ok && t <= 7; v.type = static_cast<DescriptorType>(t);
        u32(v.count); str(v.name);
        u8(v.image.dim); b(v.image.depth); b(v.image.arrayed); b(v.image.ms); u8(v.image.sampled); u32(v.image.format); e8(v.image.component, 4);
        e8(v.nvn.source, static_cast<unsigned>(NvnSource::HandleInUniform));
        i32(v.nvn.index); i32(v.nvn.bank); i32(v.nvn.word); i32(v.nvn.sb_slot); i32(v.nvn.c0_offset); b(v.nvn.default_bank);
        b(v.used); b(v.may_write);
    }
    void run(ShaderReflection& r) {
        e8(r.stage, static_cast<unsigned>(ShaderStage::TessEval));
        str(r.entry);
        for (uint32_t& x : r.local_size) u32(x);
        vec(r.bindings, [&](DescriptorBinding& x) { binding(x); });
        vec(r.inputs, [&](ShaderIo& x) { io(x); });
        vec(r.outputs, [&](ShaderIo& x) { io(x); });
        for (auto* v : {&r.builtins_in, &r.builtins_out, &r.builtins_used_in, &r.builtins_used_out}) vec(*v, [&](uint32_t& x) { u32(x); });
        b(r.uses_support_buffer);
    }
};

std::string cat_encode(const ShaderReflection& r) {
    std::string s;
    CatCodec c;
    c.out = &s;
    c.run(const_cast<ShaderReflection&>(r));   // writing reads the fields only
    return s;
}
}  // namespace

// pack_open(), lock_ held, the pack's index read.
void ShaderCache::cat_open() {
    cat_index_.assign(pack_count_, ~0u);
    cat_on_ = cat_enabled();
    if (!cat_on_) return;
    std::string f;
    if (!read_file(pack_path(dir_, ".refl"), f)) return;
    CatHeader h = {};
    const auto bad = [&](const char* why) {
        NXLOG_W("nvn", "reflection catalogue: %s: shaders are reflected (and the catalogue written again)", why);
        cat_bytes_.clear();
        cat_offs_.clear();
        cat_index_.assign(pack_count_, ~0u);
    };
    if (f.size() < sizeof h + 8) return bad("too short");
    std::memcpy(&h, f.data(), sizeof h);
    if (std::memcmp(h.magic, kCatMagic, sizeof h.magic) || h.schema != kCatSchema) return bad("another schema");
    if (h.pack_count != pack_count_ || h.pack_digest != pack_digest_ || h.pack_size != pack_size_) return bad("made for another preload.pack");
    const uint64_t want = sizeof h + 4ull * h.pack_count + 4ull * (uint64_t{h.records} + 1) + h.record_bytes + 8;
    if (want != f.size()) return bad("wrong size");
    uint64_t sum = 0;
    std::memcpy(&sum, f.data() + f.size() - 8, 8);
    if (fnv1a64(std::string_view(f.data(), f.size() - 8)) != sum) return bad("checksum mismatch");
    const char* at = f.data() + sizeof h;
    std::vector<uint32_t> index(h.pack_count), offs(uint64_t{h.records} + 1);
    std::memcpy(index.data(), at, 4ull * h.pack_count);
    at += 4ull * h.pack_count;
    std::memcpy(offs.data(), at, 4ull * offs.size());
    at += 4ull * offs.size();
    for (uint32_t r : index)
        if (r != ~0u && r >= h.records) return bad("record number out of range");
    if (offs[0] != 0 || offs.back() != h.record_bytes) return bad("record offsets out of range");
    for (size_t i = 0; i + 1 < offs.size(); i++)
        if (offs[i] > offs[i + 1]) return bad("record offsets out of order");
    const size_t base = static_cast<size_t>(at - f.data());
    for (uint32_t& o : offs) o += static_cast<uint32_t>(base);
    cat_bytes_ = std::move(f);
    cat_index_ = std::move(index);
    cat_offs_ = std::move(offs);
    size_t have = 0;
    for (uint32_t r : cat_index_) have += r != ~0u;
    NXLOG_I("nvn", "reflection catalogue: %zu of %u pack entries in %u records (%zu KiB)", have, pack_count_, h.records, cat_bytes_.size() >> 10);
}

bool ShaderCache::cat_has(const std::string& hash) const {
    auto it = pack_.find(hash);
    return it != pack_.end() && cat_index_[it->second.slot] != ~0u;
}

// The prepared entry for a pack shader from the catalogue, the resources record applied; no SPIR-V read. Null: not there, stale, or a
// record that does not decode or apply (the caller then takes the parser's path, which reports a real problem).
std::unique_ptr<ShaderCache::Preloaded> ShaderCache::cat_preload(const std::string& hash, const std::string* record) {
    if (!cat_on_ || cat_offs_.empty()) return nullptr;
    auto it = pack_.find(hash);
    if (it == pack_.end()) return nullptr;
    const uint32_t r = cat_index_[it->second.slot];
    if (r == ~0u || pack_stale(hash)) return nullptr;
    auto p = std::make_unique<Preloaded>();
    CatCodec c;
    c.p = reinterpret_cast<const uint8_t*>(cat_bytes_.data()) + cat_offs_[r];
    c.end = reinterpret_cast<const uint8_t*>(cat_bytes_.data()) + cat_offs_[r + 1];
    c.run(p->refl);
    if (!c.ok || c.p != c.end) {
        NXLOG_W("nvn", "reflection catalogue: record %u (shader %s) does not decode: reflected instead", r, hash.c_str());
        return nullptr;
    }
    if (cat_verify()) {
        std::string bytes;
        ShaderReflection fresh;
        if (pack_read(hash, bytes) && reflect_spirv(reinterpret_cast<const uint32_t*>(bytes.data()), bytes.size() / 4, fresh, nullptr) &&
            cat_encode(fresh) != cat_encode(p->refl)) {
            NXLOG_E("nvn", "reflection catalogue: shader %s: the catalogue's record differs from the parser's (parser used)", hash.c_str());
            p->refl = std::move(fresh);
        }
    }
    if (record) {
        std::vector<HarnessResource> list;
        if (!parse_harness_resources(*record, list) || !apply_harness(p->refl, list)) return nullptr;
    }
    p->from = SpirvFrom::Pack;
    p->word_count = it->second.usize / 4;
    cat_hits_.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void ShaderCache::cat_note(const std::string& hash, const ShaderReflection& refl) {
    if (!cat_on_) return;
    auto it = pack_.find(hash);
    if (it == pack_.end() || cat_index_[it->second.slot] != ~0u) return;
    std::string rec = cat_encode(refl);
    std::lock_guard lk(cat_mu_);
    cat_new_.emplace(it->second.slot, std::move(rec));
}

// The old records and this run's new ones, identical records stored once, to <pack>.refl.new and renamed over <pack>.refl.
void ShaderCache::cat_write() {
    std::unordered_map<uint32_t, std::string> fresh;
    {
        std::lock_guard lk(cat_mu_);
        fresh.swap(cat_new_);
    }
    if (!cat_on_ || fresh.empty() || !pack_file_) return;
    const uint64_t t0 = now_ms();
    std::unordered_map<std::string_view, uint32_t> ids;
    std::vector<std::string_view> recs;
    std::vector<uint32_t> index(pack_count_, ~0u);
    const auto intern = [&](std::string_view rec) {
        auto [it, added] = ids.emplace(rec, static_cast<uint32_t>(recs.size()));
        if (added) recs.push_back(rec);
        return it->second;
    };
    for (uint32_t s = 0; s < pack_count_; s++)
        if (const uint32_t r = cat_index_[s]; r != ~0u) index[s] = intern(std::string_view(cat_bytes_).substr(cat_offs_[r], cat_offs_[r + 1] - cat_offs_[r]));
    for (const auto& [s, rec] : fresh) index[s] = intern(rec);
    std::string out(sizeof(CatHeader), '\0');
    const auto put32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    for (uint32_t r : index) put32(r);
    uint32_t off = 0;
    for (std::string_view r : recs) put32(off), off += static_cast<uint32_t>(r.size());
    put32(off);
    for (std::string_view r : recs) out.append(r);
    CatHeader h = {};
    std::memcpy(h.magic, kCatMagic, sizeof h.magic);
    h.schema = kCatSchema;
    h.pack_count = pack_count_;
    h.pack_digest = pack_digest_;
    h.pack_size = pack_size_;
    h.records = static_cast<uint32_t>(recs.size());
    h.record_bytes = off;
    std::memcpy(out.data(), &h, sizeof h);
    const uint64_t sum = fnv1a64(out);
    out.append(reinterpret_cast<const char*>(&sum), 8);
    const std::string path = pack_path(dir_, ".refl"), tmp = pack_path(dir_, ".refl.new");
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(f.write(out.data(), static_cast<std::streamsize>(out.size())).flush());
    }
    if (!ok || !MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        NXLOG_W("nvn", "reflection catalogue: could not write %s", path.c_str());
        DeleteFileW(widen(tmp).c_str());
        return;
    }
    size_t have = 0;
    for (uint32_t r : index) have += r != ~0u;
    NXLOG_I("nvn", "reflection catalogue: wrote %zu of %u pack entries (%zu new) in %zu records, %zu KiB, %.0f ms", have, pack_count_, fresh.size(),
            recs.size(), out.size() >> 10, static_cast<double>(now_ms() - t0));
}

}  // namespace nxrt::nvn::be
