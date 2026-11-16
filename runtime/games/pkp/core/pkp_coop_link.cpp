// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op on the co-op channel (pkp_coop_internal.h, nxrt/coop_channel.h, the messages in pkp_coop_wire.h).
//
// The channel is opened once the game's session is up and stays open for the launch (it joins the next session on its own); its
// messages and member events are drained once a frame on the game thread, so every handler runs where the game's own code runs.
// Routing: the channel orders the messages of one sender only, so every rule change goes through the authority (node 0, the process
// that opened the session). A shared message is sent to the authority, which applies it (unless its own player is the origin: that
// change is applied already) and passes it on, byte for byte, to every member except the origin. The authority's own changes take
// the same path through the channel's loopback. So every process applies the changes in the authority's order, and never its own
// change twice. The other messages (the talk lock's) go to one node and are applied there.
#include "core/pkp_coop_internal.h"
#include "core/pkp_layout.h"

#include <array>
#include <cstddef>

#include "nxrt/coop_channel.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::link {

namespace {

namespace ch = nxrt::coop_channel;
constexpr const char* kTag = "pkp";

// Every message type, how it travels and the part that applies it, in one place.
struct Route {
    wire::Type type;
    bool shared;   // through the authority to every member; else to one node
    void (*apply)(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
};
constexpr std::array<Route, 0> kRoutes{};

struct State {
    bool open = false;
    uint32_t bad_version = 0;   // bit per type: a message of another version was logged
    ::Ctx* ctx = nullptr;       // the game thread's context while drain() runs the handlers
};
State& st() {
    static State s;
    return s;
}

void deliver(const Route& route, const ch::Message& m) {
    State& s = st();
    wire::Reader r(m.data, m.size);
    uint8_t origin = wire::kNobody;
    if (!r.header(&origin)) {
        const uint32_t bit = 1u << (static_cast<uint32_t>(route.type) & 31);
        if (!(s.bad_version & bit)) {
            s.bad_version |= bit;
            NXLOG_W(kTag, "Co-op: dropped message type %u from node %d: version %u, this build speaks %u", m.type, m.from,
                    m.size ? m.data[0] : 0, wire::kVersion);
        }
        return;
    }
    if (route.shared && ch::is_authority()) {
        if (origin != ch::self()) route.apply(s.ctx, m.from, origin, r);
        for (int node : ch::members())
            if (node != origin) ch::send(node, m.type, m.data, m.size);
        return;
    }
    route.apply(s.ctx, m.from, origin, r);
}

void on_member(const ch::MemberEvent& e) {
    if (e.node == ch::self()) {
        NXLOG_I(kTag, "Co-op: %s the session's channel as node %d%s", e.joined ? "in" : "out of", e.node, e.authority ? " (the authority)" : "");
        if (e.joined && e.authority != (role() == CoopRole::Host))
            NXLOG_W(kTag, "Co-op: this process is %s of the session, but its launch role is %s", e.authority ? "the authority" : "a member",
                    role() == CoopRole::Host ? "host" : "visit");
        return;
    }
    NXLOG_I(kTag, "Co-op: node %d %s the session%s", e.node, e.joined ? "joined" : "left", e.authority ? " (the authority)" : "");
}

void open_channel(State& s) {
    for (const Route& route : kRoutes)
        ch::on_message(static_cast<uint32_t>(route.type), [&route](const ch::Message& m) { deliver(route, m); });
    ch::on_member(&on_member);
    s.open = ch::open();
    NXLOG_I(kTag, "Co-op: the session is up; the co-op channel %s", s.open ? "is open" : "did not open");
}

}  // namespace

void tick(::Ctx* c) {
    State& s = st();
    if (!s.open) {
        if (session_bits() & layout::kSessionOnline) open_channel(s);
        if (!s.open) return;
    }
    s.ctx = c;
    ch::drain();
    s.ctx = nullptr;
}

void reset() {
    State& s = st();
    if (s.open) ch::close();
    for (const Route& route : kRoutes) ch::on_message(static_cast<uint32_t>(route.type), nullptr);
    ch::on_member(nullptr);
    s = State{};
}

uint8_t self() {
    const int node = ch::self();
    return node == ch::kNoNode ? wire::kNobody : static_cast<uint8_t>(node);
}

bool in_session() { return st().open && ch::authority() != ch::kNoNode && ch::self() != ch::kNoNode; }

bool is_authority() { return in_session() && ch::is_authority(); }

int authority() { return ch::authority(); }

bool to_authority(wire::Type type, std::vector<uint8_t>&& payload) {
    return in_session() && ch::send(ch::authority(), static_cast<uint32_t>(type), std::move(payload));
}

bool share(wire::Type type, std::vector<uint8_t>&& payload) { return to_authority(type, std::move(payload)); }

bool to_node(int node, wire::Type type, std::vector<uint8_t>&& payload) {
    return in_session() && ch::send(node, static_cast<uint32_t>(type), std::move(payload));
}

void to_members(wire::Type type, const std::vector<uint8_t>& payload) {
    if (in_session()) ch::send_all(static_cast<uint32_t>(type), payload.data(), payload.size());
}

}  // namespace nxrt::games::pkp::coop::link
