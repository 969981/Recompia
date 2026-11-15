// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: Story co-op holds a visitor's world-changing quests for the visitor's own world (pkp_story_hold.h).
//
// The site is in the quest pump of the 2.0.1 main, 0x11b600: for each id of the active list it reads the quest's flag (0x5d86e4) and at
// 0x11b6a4, `tbz w0, #0, <next>`, goes on to the completion (0x11bd3c: state 3, the row's second command set through 0x11c0a4) and takes
// the id out of the list (0x11be60) only when the flag is set. w22 is the quest id there. The midhook clears w0's bit 0 for a held quest,
// so both calls are skipped and nothing of the quest changes. An entry hook on 0x11bd3c would not do: 0x11be60 would still drop the id.
//
// A quest's commands are read as the game reads them: 0x11e520(id, slot 0..3, set, out), set 1 the commands of the completion, set 0
// those of the second phase (0x11c0a4's two callers); out is the 0x14-byte command, its first byte the type (1..0x3c, 0 for none).
#include "core/pkp_story_hold.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_story_guard.h"

#include <array>
#include <atomic>
#include <cstring>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kPumpFlagSite = 0x11b6a4;
constexpr uint32_t kPumpFlagWord = 0x3607fde0u;   // tbz w0, #0, 0x11b660
constexpr uint64_t kQuestIdReg = 22;
constexpr uint64_t kQuestCommand = 0x11e520;      // (id, slot, set, out)
constexpr uint8_t kVisitor = layout::kSessionOnline | layout::kSessionVisitor;
constexpr uint32_t kQuestMax = 1000;
constexpr double kNoticeSeconds = 8.0;

enum : uint8_t { kUnknown = 0, kFree = 1 };       // a quest's cached answer; any other value: held, the effect type that holds it

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t hook = 0;                            // 0: off or not installed
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    // The game thread's (the pump runs there):
    std::array<uint8_t, kQuestMax + 1> answer{};
    std::array<bool, kQuestMax + 1> told{};       // this quest's hold was logged, journaled and shown
    alignas(8) uint8_t command[0x20] = {};        // 0x11e520's out
};
State& st() {
    static State s;
    return s;
}

// The effect type that holds quest `id`, or kFree. Reads the row's eight commands through the game's own reader.
uint8_t classify(::Ctx* c, State& s, uint32_t id) {
    for (uint64_t set = 0; set < 2; ++set) {
        for (uint64_t slot = 0; slot < 4; ++slot) {
            std::memset(s.command, 0, sizeof s.command);
            guest::call(c, s.base + kQuestCommand, id, slot, set, reinterpret_cast<uint64_t>(s.command));
            if (story_effect_held(s.command[0])) return s.command[0];
        }
    }
    return kFree;
}

int hook_pump_flag(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire) || !(c->x[0] & 1)) return 0;
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    if (!guest::ok(game, layout::kStateSession + 1) || (guest::rd<uint8_t>(game + layout::kStateSession) & kVisitor) != kVisitor) return 0;
    const uint32_t id = static_cast<uint32_t>(c->x[kQuestIdReg]) & 0xffffu;
    if (id - 1 >= kQuestMax) return 0;
    if (s.answer[id] == kUnknown) s.answer[id] = classify(c, s, id);
    const uint8_t type = s.answer[id];
    if (type == kFree) return 0;
    c->x[0] &= ~1ull;   // the pump reads the flag as not set: no completion, the id stays in the active list
    if (!s.told[id]) {
        s.told[id] = true;
        NXLOG_I(kTag, "Story co-op: story quest %u is done but changes the town (effect 0x%02x): it completes in this player's own world", id, type);
        story_journal_note(id, "hold", type, 0);
        notices::post("pkp.story_hold", "Story co-op: this step finishes when you are back in your own world", kNoticeSeconds);
    }
    return 0;
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.hook || !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.answer.fill(kUnknown);
    s.told.fill(false);
    s.hook = in.hooks.add_mid(in.base + kPumpFlagSite, &hook_pump_flag);
}

// Play mode's frame (main is loaded): the first call checks the site (the midhook makes it a lifted function of its own, and the word)
// and arms the hook or removes it.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.hook || s.checked.exchange(true)) return;
    const uint64_t a = s.base + kPumpFlagSite;
    uint32_t word = 0;
    if (mem::is_mapped(a, 4)) std::memcpy(&word, reinterpret_cast<const void*>(a), 4);
    if (fn_lookup(a) != nullptr && word == kPumpFlagWord) {
        s.armed.store(true, std::memory_order_release);
        NXLOG_I(kTag, "Story co-op: a visitor's quests that change the town wait for the visitor's own world (hook at main+0x%llx)",
                static_cast<unsigned long long>(kPumpFlagSite));
        return;
    }
    s.hooks->remove(s.hook);
    s.hook = 0;
    NXLOG_W(kTag, "Story co-op: quests that change the town are not held: main+0x%llx is %s (word %08x, expected %08x)",
            static_cast<unsigned long long>(kPumpFlagSite), fn_lookup(a) ? "another instruction" : "not a hook site in the lifted game libraries",
            word, kPumpFlagWord);
}

void remove() {   // the registry has removed the hook
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    s.hook = 0;
    s.hooks = nullptr;
}

}  // namespace

bool story_effect_held(uint8_t type) {
    switch (type) {
        case 0x05: case 0x06: case 0x0f: case 0x10: case 0x26: case 0x2e: case 0x2f: case 0x37: case 0x38: case 0x39: return true;
        default: return false;
    }
}

const Feature kStoryHoldFeature = {.name = "story_hold", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
