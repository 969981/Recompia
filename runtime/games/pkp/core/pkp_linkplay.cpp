// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, Link Play "Interact everywhere" and Story co-op (pkp_linkplay.h). Based on pokopia-native (MIT) windows/src/coop/
// coop_interact.c and windows/docs/coop-interact.md, story-coop.md @555813a.
//
// The game keeps a session's permissions in a 36-bit mask (GameMgr+0x8c; bits 32-35, the byte at +0x90, are online / host / visitor) and
// rebuilds it in one function, 0xb0f60 of the 2.0.1 main, when hosting starts, when a town loads and when the session ends. Offline every
// bit but 31 is set. Online in a story area (stage type 1) the player always gets the Spectator pattern (bits 23-26 and 31: "certain moves
// and items can't be used"), whatever the host chose; online in Palette Town with Spectator Mode Off the normal one (bits 1-28 and 30). At
// 0xb1030, `movk x13, #1, lsl #32` in the story-area online block, x13 holds the pattern's bits 0-31 (0x87800000, set at 0xb1020) and the
// movk adds bit 32; x22 is the session's Spectator flag (w3 of the call, clear in every story area). The midhook there (recomp/games/pkp/
// main.sites) swaps the pattern for the normal one while the flag is clear: the mask is then exactly Palette Town's with Spectator Mode Off
// (pokopia-native measured fe ff ff 5f 03 for a host, fe ff ff 5f 05 for a visitor). Palette Town, the other type-3 towns, offline play, the
// host's Spectator setting, the session flag and the advertise data are not touched. Known gaps (pokopia-native): the visitor's join notice
// and the host's settings row still say Spectator Mode; a visitor's bag goes home with it as in Palette Town.
//
// Story co-op adds bit 0 to that pattern for every player (pkp_linkplay.h) and the talk-condition sites. What a player's story reads and
// writes is their own save: the m_story object (story quest history at +0x1060, ids 1-800, the DLC's at +0x1860; m_storyStageState,
// m_charaTownFlag) sits at save root (main+0x3561930) + 0x4a26a0, inside m_saveCommon (root +0xf8 up to m_saveStage at +0x7419a0), which is
// CMNDAT.BIN. A visitor keeps its own m_saveCommon in a session (its bag, coins and Pokédex are its own), so its story runs on its own save.
#include "core/pkp_linkplay.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/mods.h"
#include "nxrt/services_api.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

// Pokopia 2.0.1 main (build 76a489f3...): the mask builder and its story-area online block.
constexpr const char* kBuild201 = "76a489f3bd3642fa5e18bd83d32c98ba";
constexpr InteractSite kSites[] = {
    {kBuild201, 0xb0f60, 0xb1030, 0xf2c0002du},
};
// Story co-op's sites in the same main (words read from main; pokopia-native docs/story-coop.md, coop_interact.c g_story):
// - Talk: the talk handler 0x4c024c gives an online player who is not the host a visitor's reply; at 0x4c06d8 (`orr w8, w25, w0`) w0 is
//   IsHost's answer, and x0 = 1 there makes a visitor take the host's path;
// - Role: the mask builder's first instruction after the frame (`mov x19, x0`): the builder starts, the story is not kept until the mask
//   site keeps it;
// - NotOnline: conditions 0x19 (0x4cfe10: `bl IsOnline; mov w8, #1; bic w0, w8, w0`, the site is the mov) and 0x60 (0x4d4844: `bl IsOnline;
//   tbnz w0, #0, ...`, the site is the tbnz): x0 = 0 there is IsOnline's "no";
// - Online: condition 0x22 is the one instruction `b IsOnline` (0x4d04fc, reached through the condition table): the entry hook answers no.
constexpr StorySite kStory201[] = {
    {0x4c06d8, 0x2a000328u, StorySiteKind::Talk, false},
    {0xb0f74, 0xaa0003f3u, StorySiteKind::Role, false},
    {0x4cfe20, 0x52800028u, StorySiteKind::NotOnline, false},
    {0x4d4860, 0x37001da0u, StorySiteKind::NotOnline, false},
    {0x4d04fc, 0x17ed40adu, StorySiteKind::Online, true},
};
constexpr size_t kStoryCount = sizeof kStory201 / sizeof kStory201[0];
constexpr uint64_t kSpectatorFlagReg = 22, kPatternReg = 13;
constexpr uint64_t kNormalPattern = 0x5ffffffeull;   // bits 1-28 and 30: Palette Town, Spectator Mode Off
constexpr uint64_t kStoryBit = 1;                    // bit 0: the story system (the per-frame story update, the quest pump)

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;              // the registry's set for Link Play (pkp_feature.h)
    uint32_t hook = 0;                     // 0: not installed
    uint32_t story_hooks[kStoryCount] = {};   // all 0: Story co-op off or not installed
    bool story_on = false;                 // install registered the story hooks
    std::atomic<bool> story_armed{false};
    std::atomic<bool> story_kept{false};   // the last mask build kept this player's story system (a story area, online, no Spectator)
    std::atomic<bool> logged_talk{false};
    std::atomic<bool> logged_kept{false};
    std::atomic<bool> logged_offline{false};
    std::atomic<bool> armed{false};        // main checked: the hook may change the mask
    std::atomic<bool> checked{false};      // the frame callback made its decision
    std::atomic<bool> logged_change{false};
    std::atomic<uint64_t> changes{0};
};
// The co-op mods fingerprint's names (services ldnapi::set_coop_mods): our own features first (Story co-op while armed), then the Modtopia
// gameplay mods that are on, "id@version" sorted (nxrt/mods.h; Modtopia HOST.md section 3, Link Play). Without mods: our list alone.
std::vector<std::string> coop_mods(bool story) {
    std::vector<std::string> names;
    if (story) names.emplace_back(kStoryName);
    const std::vector<std::string> mods = nxrt::mods::gameplay_mods();
    names.insert(names.end(), mods.begin(), mods.end());
    return names;
}

State& st() {
    static State s;
    return s;
}

bool same_build(const std::string& build_id, const char* build) { return build_id.compare(0, std::strlen(build), build) == 0; }

// main+off's facts: whether a midhook there is lifted (it makes its address a lifted function of its own) and the instruction word.
void site_facts(uint64_t base, uint64_t off, bool* lifted, uint32_t* word) {
    const uint64_t a = base + off;
    *lifted = fn_lookup(a) != nullptr;
    *word = 0;
    if (mem::is_mapped(a, 4)) std::memcpy(word, reinterpret_cast<const void*>(a), 4);
}

int hook_mask(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return 0;
    const bool story = s.story_armed.load(std::memory_order_acquire) && !(c->x[kSpectatorFlagReg] & 1);
    if (interact_apply(c->x, story)) {
        s.changes.fetch_add(1, std::memory_order_relaxed);
        if (!s.logged_change.exchange(true))
            NXLOG_I(kTag, "Interact everywhere: this story area's Link Play session has Palette Town's permissions (Spectator Mode lifted)");
    }
    s.story_kept.store(story, std::memory_order_release);
    if (story && !s.logged_kept.exchange(true))
        NXLOG_I(kTag, "Story co-op: this player's own story keeps running in the Link Play session (story system on, own save)");
    return 0;
}

int hook_story(::Ctx* c, uint64_t, void* user) {
    State& s = st();
    if (!s.story_armed.load(std::memory_order_acquire)) return 0;
    switch (static_cast<StorySiteKind>(reinterpret_cast<uintptr_t>(user))) {
        case StorySiteKind::Talk:
            if (!(c->x[0] & 1) && !s.logged_talk.exchange(true)) NXLOG_I(kTag, "Story co-op: a visitor's talk takes the host's path");
            c->x[0] = 1;   // IsHost: yes
            return 0;
        case StorySiteKind::Role:
            s.story_kept.store(false, std::memory_order_release);   // the builder starts; its mask site keeps the story again or not
            return 0;
        case StorySiteKind::NotOnline:
        case StorySiteKind::Online:
            break;
    }
    if (!s.story_kept.load(std::memory_order_acquire)) return 0;
    if (!s.logged_offline.exchange(true)) NXLOG_I(kTag, "Story co-op: talks are picked from this player's own story (read as offline)");
    c->x[0] = 0;   // NotOnline: IsOnline's answer is no; Online: the condition's own answer is no
    return static_cast<StorySiteKind>(reinterpret_cast<uintptr_t>(user)) == StorySiteKind::Online ? 1 : 0;
}

const char* check_str(InteractCheck c) {
    switch (c) {
        case InteractCheck::Ok: return "ok";
        case InteractCheck::UnknownBuild: return "main is not a supported build (2.0.1 only)";
        case InteractCheck::NotLifted: return "the lifted game libraries lack the hook site (rebuild them)";
        case InteractCheck::WordDiffers: return "the instruction at the site is not the expected one";
    }
    return "?";
}

}  // namespace

const InteractSite* interact_site(const std::string& build_id) {
    for (const InteractSite& s : kSites)
        if (same_build(build_id, s.build)) return &s;
    return nullptr;
}

const StorySite* story_sites(const std::string& build_id, size_t* count) {
    if (same_build(build_id, kBuild201)) {
        *count = kStoryCount;
        return kStory201;
    }
    *count = 0;
    return nullptr;
}

InteractCheck interact_check(const std::string& build_id, bool site_lifted, uint32_t word, const InteractSite** site) {
    const InteractSite* s = interact_site(build_id);
    if (site) *site = s;
    if (!s) return InteractCheck::UnknownBuild;
    if (!site_lifted) return InteractCheck::NotLifted;
    if (word != s->word) return InteractCheck::WordDiffers;
    return InteractCheck::Ok;
}

bool interact_apply(uint64_t* x, bool keep_story) {
    if (x[kSpectatorFlagReg] & 1) return false;           // the session's own Spectator flag: left as the game has it
    const uint64_t want = kNormalPattern | (keep_story ? kStoryBit : 0);
    if (x[kPatternReg] == want) return false;
    x[kPatternReg] = want;                                // the movk that follows adds bit 32 (online)
    return true;
}

namespace {

void remove_story_hooks(State& s) {
    for (uint32_t& h : s.story_hooks) {
        if (h && s.hooks) s.hooks->remove(h);
        h = 0;
    }
    s.story_on = false;
}

// Registers the midhook at main+site when [pkp] link_interact_everywhere (or link_story_coop) is on, and Story co-op's hooks with it (main
// is not loaded yet, so nothing is checked here) and logs the choice. The hooks do nothing until linkplay_arm() / the story check accepted main.
void install(const FeatureInstall& in) {
    State& s = st();
    if (s.hook) return;
    bool on = settings::get_bool(settings::Key::LinkInteractEverywhere);
    const bool story = settings::get_bool(settings::Key::LinkStoryCoop);
    const uint64_t base = in.base;
    s.base = base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.logged_change.store(false);
    s.changes.store(0);
    s.story_armed.store(false);
    s.story_kept.store(false);
    s.logged_talk.store(false);
    s.logged_kept.store(false);
    s.logged_offline.store(false);
    services::ldnapi::set_session_variant(0, kInteractName);
    services::ldnapi::set_coop_mods(coop_mods(false));
    bool want_story = story;
    if (want_story && !services::ldnapi::native_protocol()) {
        NXLOG_W(kTag, "Story co-op ([pkp] link_story_coop): ignored, it needs [net] protocol = native (the relay Link Play has no such setting)");
        want_story = false;
    }
    if (want_story && !on) {
        NXLOG_I(kTag, "Story co-op ([pkp] link_story_coop): turns Interact everywhere on with it, as pokopia-native's does");
        on = true;
    }
    if (!on) {
        NXLOG_I(kTag, "Interact everywhere ([pkp] link_interact_everywhere): off, Link Play outside Palette Town keeps the game's Spectator Mode");
        return;
    }
    s.hook = in.hooks.add_mid(base + kSites[0].site, &hook_mask);
    NXLOG_I(kTag, "Interact everywhere ([pkp] link_interact_everywhere): on, hook at main+0x%llx, main is checked once the game runs",
            static_cast<unsigned long long>(kSites[0].site));
    if (want_story) {
        for (size_t i = 0; i < kStoryCount; ++i) {
            const StorySite& site = kStory201[i];
            void* kind = reinterpret_cast<void*>(static_cast<uintptr_t>(site.kind));
            s.story_hooks[i] = site.entry ? in.hooks.add_entry(base + site.site, &hook_story, kind)
                                          : in.hooks.add_mid(base + site.site, &hook_story, kind);
        }
        s.story_on = true;
        NXLOG_I(kTag, "Story co-op ([pkp] link_story_coop): on, every player plays their own story (%zu hooks from main+0x%llx), main is checked "
                "once the game runs", kStoryCount, static_cast<unsigned long long>(kStory201[0].site));
    }
}

// Checks Story co-op's sites in main (after Interact everywhere, which it builds on) and arms the hooks or removes them; logs once.
void story_arm(const std::string& build_id) {
    State& s = st();
    if (!s.story_on) return;
    const std::string shown = build_id.substr(0, 32);
    size_t n = 0;
    const StorySite* sites = story_sites(build_id, &n);
    InteractCheck c = (!s.armed.load() || !sites) ? InteractCheck::UnknownBuild : InteractCheck::Ok;
    const StorySite* bad = nullptr;
    uint32_t bad_word = 0;
    for (size_t i = 0; c == InteractCheck::Ok && i < n; ++i) {
        bool lifted = false;
        uint32_t word = 0;
        site_facts(s.base, sites[i].site, &lifted, &word);
        // An entry hook's address is a lifted function with or without the hook; it comes in the same relift as the midhooks, checked here.
        if (!sites[i].entry && !lifted) c = InteractCheck::NotLifted;
        else if (word != sites[i].word) c = InteractCheck::WordDiffers;
        if (c != InteractCheck::Ok) {
            bad = &sites[i];
            bad_word = word;
        }
    }
    if (c == InteractCheck::Ok) {
        services::ldnapi::set_coop_mods(coop_mods(true));
        s.story_armed.store(true, std::memory_order_release);
        NXLOG_I(kTag, "Story co-op: armed (%zu sites, build %s); Link Play sessions are refused to players without it (mods fingerprint)", n,
                shown.c_str());
        return;
    }
    s.story_armed.store(false);
    remove_story_hooks(s);
    if (c == InteractCheck::WordDiffers)
        NXLOG_W(kTag, "Story co-op: not applied: the word at main+0x%llx is %08x, expected %08x (build %s)",
                static_cast<unsigned long long>(bad->site), bad_word, bad->word, shown.c_str());
    else if (c == InteractCheck::NotLifted)
        NXLOG_W(kTag, "Story co-op: not applied: %s (main+0x%llx, build %s)", check_str(c), static_cast<unsigned long long>(bad->site),
                shown.c_str());
    else
        NXLOG_W(kTag, "Story co-op: not applied: %s (build %s)", s.armed.load() ? check_str(c) : "Interact everywhere is not armed",
                shown.empty() ? "unknown" : shown.c_str());
}

}  // namespace

InteractCheck linkplay_arm(const std::string& build_id, bool site_lifted, uint32_t word) {
    State& s = st();
    if (!s.hook) return InteractCheck::Ok;
    s.checked.store(true);
    const InteractSite* site = nullptr;
    const InteractCheck c = interact_check(build_id, site_lifted, word, &site);
    const std::string shown = build_id.substr(0, 32);
    if (c == InteractCheck::Ok) {
        services::ldnapi::set_session_variant(kInteractVariant, kInteractName);
        s.armed.store(true, std::memory_order_release);
        NXLOG_I(kTag, "Interact everywhere: armed at main+0x%llx (build %s); Link Play sessions are refused to players without it (session variant %u)",
                static_cast<unsigned long long>(site->site), shown.c_str(), kInteractVariant);
        return c;
    }
    s.armed.store(false);
    s.hooks->remove(s.hook);
    s.hook = 0;
    services::ldnapi::set_session_variant(0, kInteractName);
    if (c == InteractCheck::WordDiffers)
        NXLOG_W(kTag, "Interact everywhere: not applied: the word at main+0x%llx is %08x, expected %08x (build %s): Link Play keeps the game's rules",
                static_cast<unsigned long long>(site->site), word, site->word, shown.c_str());
    else
        NXLOG_W(kTag, "Interact everywhere: not applied: %s (build %s): Link Play keeps the game's rules", check_str(c),
                shown.empty() ? "unknown" : shown.c_str());
    return c;
}

namespace {

// Once per frame (play mode's frame: main is loaded): the first call checks main (its build id, the lifted sites, the instruction words)
// and arms the hooks or removes them; logs once either way.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.hook || s.checked.load(std::memory_order_relaxed)) return;
    const Module* m = module_at(s.base);
    const std::string build = m ? m->build_id() : std::string();
    const InteractSite* site = interact_site(build);
    bool lifted = false;
    uint32_t word = 0;
    if (site) site_facts(s.base, site->site, &lifted, &word);
    linkplay_arm(build, lifted, word);
    story_arm(build);
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.story_armed.store(false);
    s.story_kept.store(false);
    for (uint32_t& h : s.story_hooks) h = 0;
    s.story_on = false;
    services::ldnapi::set_coop_mods({});
    s.hook = 0;
    s.hooks = nullptr;
    s.checked.store(false);
    services::ldnapi::set_session_variant(0, kInteractName);
}

}  // namespace

const Feature kLinkplayFeature = {.name = "linkplay", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
