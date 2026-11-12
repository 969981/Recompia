// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host (nxrt/mods.h): the Modtopia core's calls in the order its docs/HOST.md section 3 gives, the hook points it built into the
// game exe registered as runtime hooks, the core events and the Link Play list. The backend the core calls back is modtopia_backend.cpp.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "nxrt/mods.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <vector>

#include "modtopia_mappings.gen.h"
#include "mods_internal.h"
#include "nxrt/file_write.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "nxrt/text.h"

namespace stdfs = std::filesystem;

namespace nxrt::mods {

namespace detail {
Host& host() {
    static Host h;
    return h;
}
}  // namespace detail

namespace {

using detail::Built;
using detail::host;
using detail::Stop;

constexpr const char* kTag = "mods";

std::string utf8_of(const stdfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The embedded mappings (third_party/modtopia/mappings) into dir, each file written when it is missing or its bytes differ (the launcher
// writes the same files from its own copy).
void write_mappings(const std::string& dir) {
    for (const ModtopiaMapping& m : kModtopiaMappings) {
        const stdfs::path p = stdfs::u8path(dir) / m.name;
        const std::string_view want(reinterpret_cast<const char*>(m.data), m.size);
        std::ifstream in(p, std::ios::binary);
        const std::string have = in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string();
        in.close();
        if (have == want) continue;
        std::string err;
        if (!write_file_atomic(utf8_of(p), want, &err)) NXLOG_W(kTag, "mods: cannot write the mappings %s: %s", utf8_of(p).c_str(), err.c_str());
    }
}

// The hook points the game build has ("modtopia hooks 1", one `module offset kind word site` line a point; the site column names the
// lifter site that serves the plan's kind, which is the kind the runtime hook must be registered as). Missing file: none.
size_t read_built(const std::string& path, const MtBackend& be) {
    std::ifstream in(stdfs::u8path(path), std::ios::binary);
    if (!in) return 0;
    size_t n = 0;
    for (std::string line; std::getline(in, line);) {
        const std::string t = text::trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::istringstream f(t);
        std::string module, offset, kind, word, site;
        if (!(f >> module >> offset >> kind >> word >> site)) {
            NXLOG_W(kTag, "mods: %s: a line without its site column is left out: %s", path.c_str(), t.c_str());
            continue;
        }
        Built b;
        b.plan_kind = kind == "observe" ? MT_HOOK_OBSERVE : kind == "take_over" ? MT_HOOK_TAKE_OVER : kind == "replace" ? MT_HOOK_REPLACE : 0;
        bool site_ok = true;
        if (site == "inline") b.site = HookKind::Inline;
        else if (site == "midhook") b.site = HookKind::Mid;
        else if (site == "entryhook") b.site = HookKind::Entry;
        else site_ok = false;
        const uint64_t base = be.module_base(be.user, module.c_str());
        const uint64_t off = std::strtoull(offset.c_str(), nullptr, 0);
        if (!b.plan_kind || !site_ok || !base) {
            NXLOG_W(kTag, "mods: %s: a line this host cannot use is left out: %s", path.c_str(), t.c_str());
            continue;
        }
        host().built[base + off] = b;
        ++n;
    }
    return n;
}

// The runtime hooks at the built points: each runs the core's handlers with the stopped thread's registers. "Handled" (take over, replace)
// returns from the game function. An entry stop that call_original asked to skip carries on into the function this once.
void on_inline(::Ctx* c, uint64_t addr, void*) {
    Stop s{c, addr};
    mt_hook_dispatch(host().L, addr, &s);
}
int on_mid(::Ctx* c, uint64_t addr, void*) {
    Stop s{c, addr};
    return mt_hook_dispatch(host().L, addr, &s) == MT_HANDLED ? 1 : 0;
}
int on_entry(::Ctx* c, uint64_t addr, void*) {
    if (detail::take_skip(addr)) return 0;
    Stop s{c, addr};
    return mt_hook_dispatch(host().L, addr, &s) == MT_HANDLED ? 1 : 0;
}

// One runtime hook at every address a mod that is on uses and the game build has, by the site that was built there.
void register_hooks(MtLoader* L) {
    std::vector<uint64_t> done;
    for (int i = 0, n = mt_loader_point_count(L); i < n; ++i) {
        uint64_t addr = 0;
        int kind = 0;
        const char *mod = nullptr, *name = nullptr;
        if (mt_loader_point(L, i, &addr, &kind, &mod, &name) != MT_OK || !addr) continue;
        if (std::find(done.begin(), done.end(), addr) != done.end()) continue;
        const auto it = host().built.find(addr);
        if (it == host().built.end()) continue;   // not built in: the core turns the mod off at start (hook_built_in)
        done.push_back(addr);
        switch (it->second.site) {
            case HookKind::Inline: hook_add_inline(addr, &on_inline); break;
            case HookKind::Mid: hook_add_mid(addr, &on_mid); break;
            case HookKind::Entry: hook_add_entry(addr, &on_entry); break;
        }
    }
}

// The data mods' folders over the game's RomFS (index 0 highest), under the player's own [fs] romfs_mod.
void lay_overlay(MtLoader* L) {
    std::vector<std::string> roots;
    for (int i = 0, n = mt_loader_overlay_count(L); i < n; ++i)
        if (const char* r = mt_loader_overlay_root(L, i)) roots.emplace_back(r);
    services::fsapi::set_romfs_overlays(roots);
}

void on_save_committed() {
    detail::Host& h = host();
    std::lock_guard<std::mutex> lk(h.events_mu);
    mt_loader_save_written(h.L);
}

}  // namespace

void start(const StartOptions& opt) {
    detail::Host& h = host();
    const game::Info& gi = game::info();
    if (!gi.mods || h.L) return;
    std::error_code ec;
    const stdfs::path mods_dir = stdfs::absolute(stdfs::u8path(gi.mods->mods_dir), ec);
    if (ec || !stdfs::is_directory(mods_dir, ec)) {
        NXLOG_I(kTag, "mods: no %s folder, none loaded", gi.mods->mods_dir);
        return;
    }
    h.mods_dir = utf8_of(mods_dir);
    h.mappings_dir = utf8_of(stdfs::absolute(stdfs::u8path(gi.mods->mappings_dir), ec));
    h.host_name = std::string(gi.mods->host) + (opt.host_version && *opt.host_version ? std::string(" ") + opt.host_version : std::string());
    h.game_version = gi.version;
    const Module* main = module_by_name("main");
    h.build_id = main ? main->build_id() : std::string();
    h.save_dir = services::save_dir();
    write_mappings(h.mappings_dir);

    // HOST.md section 3, before the game's first instruction: find and resolve the mods, lay the data over the RomFS, start the code. The
    // game's files are read from its folders, so the patches and the data check run now too (step 4), before the game opens its RomFS.
    const MtBackend be = detail::make_backend();
    MtOptions o{};
    o.mods_dir = h.mods_dir.c_str();
    o.mappings_dir = h.mappings_dir.c_str();
    MtLoader* L = mt_loader_new(&o, &be);
    if (!L) {
        NXLOG_E(kTag, "mods: the mod loader could not start");
        return;
    }
    if (mt_loader_scan(L) < 0) {
        NXLOG_E(kTag, "mods: %s cannot be read, none loaded", h.mods_dir.c_str());
        mt_loader_free(L);
        return;
    }
    h.L = L;
    const size_t built = read_built(gi.mods->hooks_built, be);
    register_hooks(L);
    lay_overlay(L);
    const int code = mt_loader_start(L);
    if (mt_loader_apply_patches(L) > 0) lay_overlay(L);
    mt_loader_check_data(L);
    services::fsapi::romfs_game_release();
    services::fsapi::set_save_commit_listener(&on_save_committed);

    int on = 0;
    const int found = mt_loader_count(L);
    for (int i = 0; i < found; ++i)
        if (const MtModInfo* m = mt_loader_info(L, i); m && m->state == MT_STATE_OK) ++on;
    NXLOG_I(kTag, "mods: %d found, %d on, %d code; hooks built: %zu", found, on, code, built);
}

void frame() {
    if (MtLoader* L = host().L) mt_loader_frame(L);
}

std::vector<std::string> gameplay_mods() {
    std::vector<std::string> out;
    if (MtLoader* L = host().L)
        mt_loader_gameplay_mods(L, [](void* user, const char* id) { static_cast<std::vector<std::string>*>(user)->emplace_back(id); }, &out);
    std::sort(out.begin(), out.end());
    return out;
}

MtLoader* loader() { return host().L; }

}  // namespace nxrt::mods
