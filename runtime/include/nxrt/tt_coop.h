// SPDX-License-Identifier: GPL-3.0-or-later
// Test tooling of the co-op channel (nxrt/coop_channel.h): a message recorder, a message injector with fault injection, and a log of the
// game's shared co-op state, so one process can play the receiver of a session without a second game. Built only with NXRT_TESTTOOLS
// (src/testtools/tt_coop.cpp); every caller tests that define, so a release build has none of it.
//
// Message log: what the recorder writes and the injector reads. UTF-8 text, one message per line, '#' starts a comment line:
//   <ms> <drain> <dir> <node> <type> <payload>
//   ms       host milliseconds since the log was opened
//   drain    how many drains the channel had run then (the game layer drains once a game frame)
//   dir      in    received from another member (what a replay feeds)
//            loop  our own message through the loopback (the authority's own changes)
//            out   sent by this process to node (-1: every member, send_all)
//            inj   delivered by the injector
//   node     the sender (in, loop, inj) or the receiver (out)
//   type     the message type id (decimal)
//   payload  the message bytes in hex, exactly as sent (the game's own header included); "-" when empty
// The first line is a comment naming the format: "# nxrt coop channel log 1".
//
// Injected messages are delivered by the channel's drain() (the thread the game layer drains on, at the point it drains), through the
// handler registered for their type, after the messages the channel itself received in that drain: to the game layer they are messages
// from `node`. They need the channel open (the game layer opened it); a drain with no channel delivers nothing and keeps them.
//
// Configuration ([test] section, env NXRT_TEST_<KEY>; read at first use, independent of the other [test] keys):
//   coop_record=<file>       write every message this process sends, receives or is injected to <file> (the format above)
//   coop_state=<file>        where the shared-state lines go (default: <out>/coop_state.txt; [test] out, default testout)
//   coop_state_every=<n>     a state line every n game frames when the state changed since the last line (0, the default: only at
//                            @coop_state steps)
// Script steps (tt_script.h):
//   @coop_msg <type> <node> <hex> [options]   inject one message: type id, sender node, the payload bytes in hex (header included)
//   @coop_inject <file> [options]             inject the messages of a message log (by default its `in` lines)
//   @coop_state [<label>]                     write one state line at the next game frame, labelled
// Options (key=value; counters are 1-based over the messages of that one step, deterministic):
//   dir=in|loop|out|inj|all   which lines of the file (default in)          node=<n>     every message from this node instead
//   pace=rec|none             keep the recorded ms spacing (default for a file) or deliver at once (default for @coop_msg)
//   delay=<n>                 deliver n drains (frames) later than due       dup=<n>      every n-th message is delivered twice
//   drop=<n>                  every n-th message is dropped                  reorder=<w>  shuffle within each window of w messages
//   seed=<n>                  the shuffle's seed (default 1)
// State line (coop_state file and the log):
//   state <frame> <label> all=<hash> <section>=<hash> ...      then one "  <section> <summary>" line per section
// frame counts game frames since the first state_frame call; hashes are FNV-1a 64 over each section's text, all= over every section.
// Two runs are in sync at a label when their `all` (or one section's) hashes are equal.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::tt::coop {

// ---- the channel's side (services/ldn_channel.cpp) ----
void on_sent(int node, uint32_t type, const void* data, size_t size);
void on_received(int from, bool loopback, uint32_t type, const uint8_t* data, size_t size);
// Hands every injected message that is due to deliver, in order, and counts this drain. Returns how many were delivered.
using DeliverFn = void (*)(int from, uint32_t type, const uint8_t* data, size_t size);
size_t drain_injected(DeliverFn deliver);

// ---- the game's side ----
// One part of the shared state: `text` is what must be equal on every member (hashed), `summary` a short line for people.
struct StateSection {
    std::string name, text, summary;
};
using StateFillFn = void (*)(std::vector<StateSection>* out);
// The game layer calls this once a game frame on its game thread; fill runs only when a state line is due.
void state_frame(StateFillFn fill);

// ---- the script's side (tt_script.cpp, tt_runner.cpp) ----
// text: the step without its '@' ("coop_msg 10 9 0309014803"). check: at load, the arguments; run: does it (queues the messages).
bool check_step(const std::string& text, std::string* err);
bool run_step(const std::string& text, std::string* err);

}  // namespace nxrt::tt::coop
