// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] shader_key_check (src/gfx/nvn/nvn_key_check.cpp): the compare and log-once logic of KeyChecker, with a fake re-hash (the first byte of
// the code block) so the test does not need real control sections.
#include <cstdint>
#include <string>

#include "nvn_key_check.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_key_check)

using namespace nxrt::nvn;

namespace {
bool fake_rehash(const uint8_t* code, const uint8_t* control, uint64_t* key) {
    if (!control[0]) return false;   // an unreadable control section
    *key = 0x1000 + code[0];
    return true;
}

KeySnapshot snap(uintptr_t id, uint8_t* code, uint8_t* ctl, int stage, uint64_t key) {
    KeySnapshot s;
    s.id = id;
    s.st[stage] = {code, ctl, key};
    return s;
}
}  // namespace

NXTEST(nvn_key_check_match_is_silent) {
    uint8_t code[1] = {7}, ctl[1] = {1};
    KeyChecker kc(1, fake_rehash);
    std::string line = "x";
    CHECK_EQ(kc.on_draw(snap(1, code, ctl, 0, 0x1007), 5, &line), 0);
    CHECK(line.empty());
    CHECK_EQ(kc.programs_checked(), 1u);
    CHECK_EQ(kc.programs_mismatched(), 0u);
}

NXTEST(nvn_key_check_mismatch_logs_once_per_program) {
    uint8_t code[1] = {7}, ctl[1] = {1};
    KeyChecker kc(1, fake_rehash);
    std::string line;
    const KeySnapshot s = snap(1, code, ctl, 1, 0x1001);   // key taken when the code started with 1; it now starts with 7
    CHECK_EQ(kc.on_draw(s, 42, &line), 1);
    CHECK(line.find("0000000000001001 -> 0000000000001007") != std::string::npos);
    CHECK(line.find("stage 1") != std::string::npos);
    CHECK(line.find("frame 42") != std::string::npos);
    CHECK_EQ(kc.programs_mismatched(), 1u);
    // First draw only (every = 1): later draws are not re-hashed and not logged.
    CHECK_EQ(kc.on_draw(s, 43, &line), 0);
    CHECK(line.empty());
    CHECK_EQ(kc.checks(), 1u);
    // Another program with the same fault logs its own line.
    CHECK_EQ(kc.on_draw(snap(2, code, ctl, 1, 0x1001), 44, &line), 1);
    CHECK(!line.empty());
    CHECK_EQ(kc.programs_mismatched(), 2u);
    CHECK_EQ(kc.stages_mismatched(), 2u);
}

NXTEST(nvn_key_check_every_nth_counts_without_relogging) {
    uint8_t code[1] = {1}, ctl[1] = {1};
    KeyChecker kc(3, fake_rehash);
    std::string line;
    const KeySnapshot s = snap(9, code, ctl, 0, 0x1001);
    CHECK_EQ(kc.on_draw(s, 1, &line), 0);   // draw 1: match
    code[0] = 2;                            // the game rewrote the block
    CHECK_EQ(kc.on_draw(s, 2, &line), 0);   // draw 2: not due
    CHECK_EQ(kc.on_draw(s, 3, &line), 0);   // draw 3: not due
    CHECK_EQ(kc.on_draw(s, 4, &line), 1);   // draw 4: due (every 3rd after the first)
    CHECK(!line.empty());
    CHECK_EQ(kc.on_draw(s, 5, &line), 0);
    CHECK_EQ(kc.on_draw(s, 6, &line), 0);
    CHECK_EQ(kc.on_draw(s, 7, &line), 1);   // due again: counted, but the line was already written
    CHECK(line.empty());
    CHECK_EQ(kc.programs_mismatched(), 1u);
    CHECK_EQ(kc.stages_mismatched(), 2u);
}

NXTEST(nvn_key_check_skips_unlinked_and_unreadable_stages) {
    uint8_t code[1] = {7}, ctl[1] = {0};
    KeyChecker kc(1, fake_rehash);
    std::string line;
    KeySnapshot s = snap(3, code, ctl, 0, 0x1001);   // control unreadable: not a mismatch
    s.st[1] = {nullptr, nullptr, 0};                 // stage without a block
    CHECK_EQ(kc.on_draw(s, 1, &line), 0);
    CHECK(line.empty());
}
