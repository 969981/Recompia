// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, Link Play's request queue on a member (pkp_coop_internal.h; RE: modding-docs coop-v2/VISITOR-SERVICES-RE.md
// section 11). The queue is u16s at block map +0x9218378: q[0] requests sent, q[1] answers taken, q[4] (+8) the strict kind in flight
// (0xffff when none). A strict send sets q[4] and the answer of that kind clears it (0x226ac0..0x226b18). The talk-end record with items
// (type 0xb, 0x2159c0) is sent only while q[0] == q[1] and q[4] >= 0x23, else dropped.
//   0x214954(q) (entry hook), the visitor's queue reset at session start (only caller 0x534c74): it zeroes q[0..3] and leaves q[4]. q[4]
//   is otherwise reset only when the block map is built (0x214890 from its constructor 0x2009a0, once per process). So a strict request
//   still out when a session ended left q[4] set in every later session of that process, and each talk-end record with items was
//   dropped. The answer of the old session never comes; here q[4] is cleared with q[0..3], as the block map's own reset does.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::linkqueue {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kStrictKind = 8;     // q[4]
constexpr uint16_t kNoKind = 0xffff;

}  // namespace

int on_session_reset(::Ctx* c) {
    if (!link::in_session() || !(session_bits() & layout::kSessionVisitor)) return 0;
    const uint64_t q = c->x[0];
    if (!guest::ok(q + kStrictKind, 2)) return 0;
    const uint16_t kind = guest::rd<uint16_t>(q + kStrictKind);
    if (kind == kNoKind) return 0;
    guest::wr<uint16_t>(q + kStrictKind, kNoKind);
    NXLOG_I(kTag, "Co-op Link Play queue: request kind %u was still out from an earlier session; cleared at session start", kind);
    return 0;   // the game's own reset zeroes q[0..3]
}

}  // namespace nxrt::games::pkp::coop::linkqueue
