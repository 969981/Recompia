// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op on the co-op channel (pkp_coop_internal.h, nxrt/coop_channel.h, the messages in pkp_coop_wire.h).
//
// The channel is opened once the game's session is up and stays open for the launch (it joins the next session on its own); its
// messages and member events are drained once a frame on the game thread, so every handler runs where the game's own code runs.
// Routing: the channel orders the messages of one sender only, so every rule change goes through the authority (node 0, the process
// that opened the session). A shared message is sent to the authority, which applies it (unless its own player is the origin: that
// change is applied already) and passes it on, byte for byte, to every member except the origin. The authority's own changes take
// the same path through the channel's loopback. So every process applies the changes in the authority's order, and never its own
// change twice. The other messages (the talk lock's) go to one node and are applied there.
#include "core/pkp_coop_internal.h"
#include "core/pkp_journal.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <string>

#include "nxrt/coop_channel.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::link {

namespace {

namespace ch = nxrt::coop_channel;
constexpr const char* kTag = "pkp";

// How a message type travels, which also says who may have sent it.
enum class Path : uint8_t {
    Shared,          // a member's (or the authority's own) change, to the authority, which passes it on to every other member
    ToAuthority,     // a member's request to the authority alone
    FromAuthority,   // the authority's word to a member
};

// Every message type, how it travels and the part that applies it, in one place.
struct Route {
    wire::Type type;
    Path path;
    void (*apply)(::Ctx* c, int from, uint8_t origin, wire::Reader& r);
};
constexpr Route kRoutes[] = {
    {wire::Type::StoryStates, Path::Shared, &story::on_states_message},
    {wire::Type::Ability, Path::Shared, &story::on_ability_message},
    {wire::Type::TalkAsk, Path::ToAuthority, &talk::on_ask},
    {wire::Type::TalkDone, Path::ToAuthority, &talk::on_done},
    {wire::Type::TalkHolder, Path::FromAuthority, &talk::on_holder},
    {wire::Type::NamePrompt, Path::Shared, &catchup::on_name_message},
    {wire::Type::Items, Path::Shared, &catchup::on_items_message},
    {wire::Type::StoryDigest, Path::FromAuthority, &story::on_digest_message},
    {wire::Type::MemberName, Path::Shared, &notice::on_name_message},
    {wire::Type::ReqUnique, Path::Shared, &requests::on_unique_message},
    {wire::Type::ReqOutcome, Path::ToAuthority, &outcomes::on_outcome_message},
    {wire::Type::RecipeKnown, Path::Shared, &recipes::on_known_message},
    {wire::Type::WildPop, Path::ToAuthority, &story::on_wild_pop_message},
    {wire::Type::QuestFlagAsk, Path::ToAuthority, &quests::on_ask_message},
    {wire::Type::Purchase, Path::ToAuthority, &purchases::on_purchase_message},
    {wire::Type::PurchaseAsk, Path::ToAuthority, &purchases::on_ask_message},
    {wire::Type::PurchaseLog, Path::FromAuthority, &purchases::on_log_message},
    {wire::Type::StockAsk, Path::ToAuthority, &stock::on_ask_message},
    {wire::Type::StockState, Path::FromAuthority, &stock::on_state_message},
    {wire::Type::ShopRows, Path::FromAuthority, &shoprows::on_rows_message},
    {wire::Type::EventHistory, Path::Shared, &events::on_history_message},
    {wire::Type::StoryLines, Path::Shared, &events::on_lines_message},
    {wire::Type::DexKnown, Path::Shared, &discovery::on_dex_message},
    {wire::Type::DexScene, Path::Shared, &discovery::on_dex_scene_message},
    {wire::Type::HabitatKnown, Path::Shared, &discovery::on_habitat_message},
    {wire::Type::HabitatScene, Path::Shared, &discovery::on_habitat_scene_message},
    {wire::Type::UnlockKnown, Path::Shared, &unlocks::on_known_message},
    {wire::Type::TutorialKnown, Path::Shared, &tutorials::on_known_message},
    {wire::Type::TipClosed, Path::Shared, &tutorials::on_closed_message},
    {wire::Type::HabitatHints, Path::Shared, &hints::on_message},
    {wire::Type::AbilityCleared, Path::Shared, &story::on_cleared_message},
    {wire::Type::AreaMusic, Path::FromAuthority, &story::on_music_message},
    {wire::Type::ChallengesClaimed, Path::FromAuthority, &money::on_claimed_message},
    {wire::Type::StateDigest, Path::FromAuthority, &digest::on_digest_message},
    {wire::Type::StateAsk, Path::ToAuthority, &digest::on_ask_message},
    {wire::Type::StateFull, Path::FromAuthority, &digest::on_full_message},
    {wire::Type::StateReport, Path::ToAuthority, &digest::on_report_message},
    {wire::Type::BagRecord, Path::ToAuthority, &bags::on_record_message},
    {wire::Type::BagRestore, Path::FromAuthority, &bags::on_restore_message},
    // the story records (pkp_coop_records.cpp)
    {wire::Type::StoryRecord, Path::FromAuthority, &records::on_record_message},
    // the story talks (pkp_coop_skits.cpp)
    {wire::Type::StorySkits, Path::FromAuthority, &skits::on_skits_message},
    // friendship
    {wire::Type::FriendAsk, Path::ToAuthority, &friendship::on_ask_message},
    {wire::Type::FriendState, Path::FromAuthority, &friendship::on_state_message},
    {wire::Type::CreatureEvent, Path::ToAuthority, &feed::on_event_message},
    {wire::Type::TravelAsk, Path::ToAuthority, &travel::on_ask_message},
    // a newly met creature moving in
    {wire::Type::NewcomerAsk, Path::ToAuthority, &newcomer::on_ask_message},
    // a member's deposit into the story's materials drop
    {wire::Type::DepositAsk, Path::ToAuthority, &deposit::on_ask_message},
    {wire::Type::DepositState, Path::FromAuthority, &deposit::on_state_message},
    // a story follow on a member
    {wire::Type::StoryFollow, Path::ToAuthority, &follow::on_ask_message},
    // the shared save
    {wire::Type::SaveHave, Path::ToAuthority, &savecopy::on_have_message},
    {wire::Type::SaveOffer, Path::FromAuthority, &savecopy::on_offer_message},
    {wire::Type::SaveChunk, Path::FromAuthority, &savecopy::on_chunk_message},
    // a creature's talk record flag a member's talk arms
    {wire::Type::TalkRecArmed, Path::ToAuthority, &talkrec::on_armed_message},
    // a dropped player is waited for
    {wire::Type::DropHere, Path::ToAuthority, &drop::on_here_message},
    {wire::Type::DropLeaving, Path::ToAuthority, &drop::on_leaving_message},
    {wire::Type::DropWait, Path::FromAuthority, &drop::on_wait_message},
    {wire::Type::DropClosing, Path::FromAuthority, &drop::on_closing_message},
    // the characters another member's scenes hold
    {wire::Type::SceneHolds, Path::Shared, &scenes::on_holds_message},
};

struct State {
    bool open = false;
    uint32_t bad_version = 0;   // bit per type: a message of another version was logged
    uint32_t bad_sender = 0;    // bit per type: a message whose sender may not send it was logged
    ::Ctx* ctx = nullptr;       // the game thread's context while drain() runs the handlers
};
State& st() {
    static State s;
    return s;
}

// The sender (m.from, set by the channel) may send this message with this origin. A process sends only its own messages (origin is
// its node; the authority's own reach it through the loopback, from itself), and only the authority passes a member's shared message
// on, byte for byte, so a shared message on a member comes from the authority with any other member as its origin. The authority is
// known before the first message of a session is drained (the channel sets it before it queues our join).
bool sender_ok(Path path, int from, uint8_t origin) {
    const int authority = ch::authority();
    switch (path) {
    case Path::Shared:
        return ch::is_authority() ? from == origin : from == authority && origin != ch::self();
    case Path::ToAuthority:
        return ch::is_authority() && from == origin;
    case Path::FromAuthority:
        return from == authority && origin == from;
    }
    return false;
}

void deliver(const Route& route, const ch::Message& m) {
    State& s = st();
    wire::Reader r(m.data, m.size);
    uint8_t origin = wire::kNobody;
    const uint32_t bit = 1u << (static_cast<uint32_t>(route.type) & 31);
    if (!r.header(&origin)) {
        wirelog::dropped(m.type, m.from, m.data, m.size, "another wire version");
        if (!(s.bad_version & bit)) {
            s.bad_version |= bit;
            NXLOG_W(kTag, "Co-op: dropped message type %u from node %d: version %u, this build speaks %u", m.type, m.from,
                    m.size ? m.data[0] : 0, wire::kVersion);
        }
        return;
    }
    if (!sender_ok(route.path, m.from, origin)) {
        wirelog::dropped(m.type, m.from, m.data, m.size, "its sender may not send it with that origin");
        if (!(s.bad_sender & bit)) {
            s.bad_sender |= bit;
            NXLOG_W(kTag, "Co-op: dropped message type %u from node %d: origin %u, authority node %d; it may not send that", m.type,
                    m.from, origin, ch::authority());
        }
        return;
    }
    if (route.path == Path::Shared && ch::is_authority()) {
        wirelog::received(m.type, m.from, origin, m.data, m.size, origin != ch::self() ? "applying here, then passing on" : "this player's own (applied already), passing on");
        if (origin != ch::self()) route.apply(s.ctx, m.from, origin, r);
        std::string to;
        for (int node : ch::members())
            if (node != origin) to += journal::fmt(" n%d%s", node, ch::send(node, m.type, m.data, m.size) ? "" : " (not taken)");
        wirelog::forwarded(m.type, origin, m.data, m.size, to.c_str());
        return;
    }
    wirelog::received(m.type, m.from, origin, m.data, m.size, "applying here");
    route.apply(s.ctx, m.from, origin, r);
}

void on_member(const ch::MemberEvent& e) {
    // Our own leave may be drained once the channel has no node for us any more.
    const bool self = e.node == ch::self() || ch::self() == ch::kNoNode;
    talk::on_member(st().ctx, e.node, e.joined, e.authority, self);
    gimmicks::on_member(e.node, e.joined, self);
    savecopy::on_member(e.node, e.joined, self);
    drop::on_member(e.node, e.joined, self);
    scenes::on_member(e.node, e.joined, self);
    if (self) {
        story::on_session_change();
        quests::on_session_change();
        notice::on_session_change();
        requests::on_session_change();
        recipes::on_session_change();
        events::on_session_change();
        discovery::on_session_change();
        unlocks::on_session_change();
        tutorials::on_session_change();
        hints::on_session_change();
        purchases::on_session_change();
        stock::on_session_change();
        shoprows::on_session_change();
        money::on_session_change();
        digest::on_session_change();
        records::on_session_change();
        skits::on_session_change();
        NXLOG_I(kTag, "Co-op: %s the session's channel as node %d%s", e.joined ? "in" : "out of", e.node, e.authority ? " (the authority)" : "");
        if (e.joined && e.authority != (role() == CoopRole::Host))
            NXLOG_W(kTag, "Co-op: this process is %s of the session, but its launch role is %s", e.authority ? "the authority" : "a member",
                    role() == CoopRole::Host ? "host" : "visit");
        return;
    }
    if (e.joined) {
        story::on_member_joined();
        notice::on_member_joined();
        requests::on_member_joined();
        recipes::on_member_joined();
        events::on_member_joined();
        discovery::on_member_joined();
        unlocks::on_member_joined();
        tutorials::on_member_joined();
        hints::on_member_joined();
        money::on_member_joined();
        stock::on_member_joined();
        shoprows::on_member_joined();
        bags::on_member_joined(e.node);
        records::on_member_joined();
        skits::on_member_joined();
        friendship::on_member_joined(e.node);
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

size_t member_count() { return st().open ? ch::members().size() : 0; }

bool is_authority() { return in_session() && ch::is_authority(); }

int authority() { return ch::authority(); }

bool to_authority(wire::Type type, std::vector<uint8_t>&& payload) { return to_node(ch::authority(), type, std::move(payload)); }

bool share(wire::Type type, std::vector<uint8_t>&& payload) { return to_authority(type, std::move(payload)); }

bool to_node(int node, wire::Type type, std::vector<uint8_t>&& payload) {
    const auto t = static_cast<uint32_t>(type);
    wirelog::Pending log = wirelog::sending(t, node, payload.data(), payload.size());
    const bool queued = in_session() && ch::send(node, t, std::move(payload));
    wirelog::sent(log, queued);
    return queued;
}

void to_members(wire::Type type, const std::vector<uint8_t>& payload) {
    if (!in_session()) return;
    const int count = ch::send_all(static_cast<uint32_t>(type), payload.data(), payload.size());
    wirelog::sent_all(static_cast<uint32_t>(type), payload.data(), payload.size(), count);
}

}  // namespace nxrt::games::pkp::coop::link
