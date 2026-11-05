// SPDX-License-Identifier: GPL-3.0-or-later
// Link Play "Interact everywhere" (games/pkp/pkp_linkplay.h): the site by main build, the check before arming, the register change at the
// site, and the register logic: the midhook only with the setting on, armed (and the ldn session variant set) only for main 2.0.1 with the
// site lifted and the expected word, removed otherwise. After pokopia-native (MIT) windows/src/coop/tests/coop_test.c test_interact @555813a.
#include "test_framework.h"

#include <cstdint>
#include <string>

#include "nxrt/hooks.h"
#include "nxrt/services_api.h"
#include "pkp_linkplay.h"

NXTEST_GROUP(pkp_linkplay)

namespace {

using namespace nxrt::games::pkp;
namespace ldnapi = nxrt::services::ldnapi;

constexpr uint64_t kBase = 0x7100000000ull, kSite = kBase + 0xb1030;
const std::string kBuild201 = "76a489f3bd3642fa5e18bd83d32c98bacc5f7d0e000000000000000000000000";   // the NSO header's module id
constexpr uint32_t kWord = 0xf2c0002du;                                                              // movk x13, #1, lsl #32
constexpr uint64_t kSpectator = 0x87800000ull, kNormal = 0x5ffffffeull;

size_t hooks_at_site() { return nxrt::hook_count_at(kSite, nxrt::HookKind::Mid); }

// the mask builder reaching the site: x13 the Spectator pattern, x22 the session's Spectator flag
uint64_t run_site(uint64_t flag) {
    ::Ctx c{};
    c.x[13] = kSpectator;
    c.x[22] = flag;
    nx_site_midhook(&c, kSite);
    return c.x[13];
}

}  // namespace

NXTEST(sites_by_build) {
    const InteractSite* s = interact_site(kBuild201);
    REQUIRE(s != nullptr);
    CHECK_EQ(s->fn, 0xb0f60ull);
    CHECK_EQ(s->site, 0xb1030ull);
    CHECK_EQ(s->word, kWord);
    CHECK(interact_site("76a489f3bd3642fa5e18bd83d32c98ba") != nullptr);   // pokopia-native's 32-digit form
    CHECK(interact_site("816c5358e8178d802fcec4a7ce5a741a") == nullptr);   // 1.0.5: none
    CHECK(interact_site("") == nullptr);
    const InteractSite* got = nullptr;
    CHECK(interact_check(kBuild201, true, kWord, &got) == InteractCheck::Ok);
    CHECK(got == s);
    CHECK(interact_check(kBuild201, false, kWord, &got) == InteractCheck::NotLifted);
    CHECK(interact_check(kBuild201, true, 0xd503201fu, &got) == InteractCheck::WordDiffers);
    CHECK(interact_check("816c5358e8178d802fcec4a7ce5a741a", true, kWord, &got) == InteractCheck::UnknownBuild);
    CHECK(got == nullptr);
}

NXTEST(register_change) {
    uint64_t x[31] = {};
    x[13] = kSpectator;
    CHECK(interact_apply(x));
    CHECK_EQ(x[13], kNormal);                 // the flag clear: Palette Town's Spectator-Off pattern
    CHECK(!interact_apply(x));                // already normal: no change counted
    x[13] = kSpectator;
    x[22] = 1;
    CHECK(!interact_apply(x));
    CHECK_EQ(x[13], kSpectator);              // the flag set: unchanged
}

NXTEST(register_logic) {
    linkplay_remove();
    // off: no hook, no variant
    linkplay_install(kBase, false);
    CHECK_EQ(hooks_at_site(), 0u);
    CHECK_EQ(ldnapi::session_variant(), 0);
    CHECK(linkplay_arm(kBuild201, true, kWord) == InteractCheck::Ok);   // nothing installed: nothing to arm
    CHECK(!linkplay_armed());
    linkplay_remove();

    // on: the hook is there at once but does nothing until main is accepted
    linkplay_install(kBase, true);
    CHECK_EQ(hooks_at_site(), 1u);
    CHECK_EQ(ldnapi::session_variant(), 0);
    CHECK_EQ(run_site(0), kSpectator);
    CHECK(linkplay_arm(kBuild201, true, kWord) == InteractCheck::Ok);
    CHECK(linkplay_armed());
    CHECK_EQ(ldnapi::session_variant(), kInteractVariant);
    CHECK_EQ(run_site(0), kNormal);
    CHECK_EQ(run_site(1), kSpectator);
    CHECK_EQ(linkplay_changes(), 1u);
    linkplay_remove();
    CHECK_EQ(hooks_at_site(), 0u);
    CHECK_EQ(ldnapi::session_variant(), 0);

    // on, but main is not the one: the hook goes away and the session stays the game's (no variant)
    const struct { std::string build; bool lifted; uint32_t word; InteractCheck want; } bad[] = {
        {kBuild201, true, 0xd503201fu, InteractCheck::WordDiffers},
        {kBuild201, false, kWord, InteractCheck::NotLifted},
        {"816c5358e8178d802fcec4a7ce5a741a", true, kWord, InteractCheck::UnknownBuild},
        {"", false, 0, InteractCheck::UnknownBuild},
    };
    for (const auto& b : bad) {
        linkplay_install(kBase, true);
        CHECK_EQ(hooks_at_site(), 1u);
        CHECK(linkplay_arm(b.build, b.lifted, b.word) == b.want);
        CHECK(!linkplay_armed());
        CHECK_EQ(hooks_at_site(), 0u);
        CHECK_EQ(ldnapi::session_variant(), 0);
        CHECK_EQ(run_site(0), kSpectator);
        linkplay_remove();
    }
}
