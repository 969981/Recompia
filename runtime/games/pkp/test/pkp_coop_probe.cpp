// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, test tool: the shared co-op state for the test tooling's state lines (nxrt/tt_coop.h). Each game frame it hands the
// tooling a reader; the tooling calls it only when a line is due. Sections, each hashed on its own (pkp_layout.h has the offsets):
//   quests     the state of quest id 1..1000 (the low byte of its quest word; the flag bit is left out: it is the pump's work in progress)
//   abilities  the 0x20 bytes of ability bits
//   requests   byte +0 (the state) of unique request record 1..200; the badge, progress and flag bytes are each player's own. A member's
//              5 on a request that is not a story step stays open on the others by design (pkp_coop_requests.cpp), so this section
//              may differ in a run that is in sync
//   names      node=name of this process (its own save) and of every member that joined (the names their MemberName messages gave)
// Reads nothing until the co-op feature is armed (coop::base() set). Runs on the game thread after co-op's own game frame (the registry
// table), so it sees the state after this frame's messages.
#include "test/pkp_coop_probe.h"
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "nxrt/coop_channel.h"
#include "nxrt/tt_coop.h"

namespace nxrt::games::pkp {

namespace {

using nxrt::tt::coop::StateSection;
constexpr size_t kAbilityBytes = coop::wire::kAbilityBytes;

uint64_t save_root() {
    if (!coop::base()) return 0;
    const uint64_t r = guest::rd<uint64_t>(coop::base() + layout::kSaveRoot);
    const uint64_t end = std::max({layout::kQuestWordsEnd, layout::kSaveAbilities + kAbilityBytes,
                                   layout::kSaveUniqueRequests + (layout::kUniqueRequestCount + 1) * 4});
    return guest::ok(r, end) ? r : 0;
}

StateSection quests(uint64_t root) {
    StateSection p{"quests", {}, {}};
    unsigned by_state[5] = {}, other = 0;
    for (uint32_t i = 0; i < layout::kQuestWordCount; ++i) {
        const uint8_t state = static_cast<uint8_t>(guest::rd<uint16_t>(root + layout::quest_word(i + 1)));
        p.text.push_back(static_cast<char>(state));
        if (state < 5) ++by_state[state];
        else ++other;
    }
    char line[96];
    std::snprintf(line, sizeof line, "listed %u started %u complete %u closed %u other %u", by_state[1], by_state[2], by_state[3],
                  by_state[4], other);
    p.summary = line;
    return p;
}

StateSection abilities(uint64_t root) {
    StateSection p{"abilities", {}, {}};
    for (size_t i = 0; i < kAbilityBytes; ++i) {
        const uint8_t b = guest::rd<uint8_t>(root + layout::kSaveAbilities + i);
        p.text.push_back(static_cast<char>(b));
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", b);
        p.summary += hex;
    }
    return p;
}

StateSection requests(uint64_t root) {
    StateSection p{"requests", {}, {}};
    for (uint32_t id = 1; id <= layout::kUniqueRequestCount; ++id) {
        const uint8_t state = guest::rd<uint8_t>(root + layout::kSaveUniqueRequests + id * 4);
        p.text.push_back(static_cast<char>(state));
        if (state >= 2) p.summary += (p.summary.empty() ? "" : " ") + std::to_string(id) + ":" + std::to_string(state);
    }
    if (p.summary.empty()) p.summary = "none above 1";
    return p;
}

StateSection names() {
    StateSection p{"names", {}, {}};
    const int self = coop_channel::self();
    std::vector<int> nodes = coop_channel::members();
    if (self != coop_channel::kNoNode) nodes.push_back(self);
    std::sort(nodes.begin(), nodes.end());
    for (const int node : nodes) {
        const std::string name = node == self ? coop::catchup::player_name() : coop::notice::name_of(static_cast<uint8_t>(node));
        p.text += std::to_string(node) + "=" + name + ";";
    }
    p.summary = p.text.empty() ? "no session" : p.text;
    return p;
}

void fill(std::vector<StateSection>* out) {
    const uint64_t root = save_root();
    if (!root) {
        out->push_back({"save", "none", "co-op is not armed or the save is not loaded"});
        return;
    }
    out->push_back(quests(root));
    out->push_back(abilities(root));
    out->push_back(requests(root));
    out->push_back(names());
}

void game_frame(::Ctx*) { tt::coop::state_frame(&fill); }

}  // namespace

const Feature kCoopProbeFeature = {.name = "coop_probe", .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
