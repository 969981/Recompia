// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt/coop_channel.h: the co-op channel's entry points. Holds the process's one channel (net/coop_channel.h) on the ports of the ldn
// backend [net] protocol picks, the handlers the game layer registered, and drain(). ldn_reset() closes it before the backends reset.
// Test-tools builds pass every message sent and drained to the recorder, and deliver the injected ones in drain() (nxrt/tt_coop.h).
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "ldn_backend.h"
#include "nxrt/coop_channel.h"
#include "nxrt/log.h"
#include "../net/coop_channel.h"
#if NXRT_TESTTOOLS
#include "nxrt/tt_coop.h"
#endif

namespace nxrt::coop_channel {
namespace {

struct Slot {
    std::mutex mu;                                      // the channel pointer
    std::shared_ptr<vnet::CoopChannel> channel;
    std::mutex handlers_mu;
    std::map<uint32_t, MessageHandler> on_message;
    MemberHandler on_member;
    std::set<uint32_t> unhandled_logged;
};
Slot& slot() {
    static Slot s;
    return s;
}

std::shared_ptr<vnet::CoopChannel> current() {
    Slot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.channel;
}

// Runs the handler of one message's type, on the calling thread.
void dispatch(int from, uint32_t type, const uint8_t* data, size_t size) {
    Slot& s = slot();
    MessageHandler h;
    {
        std::lock_guard<std::mutex> lk(s.handlers_mu);
        if (auto f = s.on_message.find(type); f != s.on_message.end()) h = f->second;
        else if (s.unhandled_logged.insert(type).second)
            NXLOG_W("net", "coop channel: no handler for message type %u (from node %d): dropped", type, from);
    }
    if (h) h(Message{from, type, data, size});
}

}  // namespace

bool open() {
    Slot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.channel) return true;
    s.channel = std::make_shared<vnet::CoopChannel>(
        [](uint16_t port) { return services::ldn_impl::selected_backend().open_port(port); });
    NXLOG_I("net", "coop channel: open (virtual port %u, %s backend)", kPort, services::ldn_impl::selected_backend().name());
    return true;
}

void close() {
    std::shared_ptr<vnet::CoopChannel> c;
    {
        Slot& s = slot();
        std::lock_guard<std::mutex> lk(s.mu);
        c.swap(s.channel);
    }
    // Outside the lock: the pump says goodbye and its port is closed when stop() returns, even while a drain on another thread
    // still holds the channel (ldn_reset relies on it: the backend destroys the port's session right after).
    if (c) c->stop();
}

bool is_open() { return current() != nullptr; }

void on_message(uint32_t type, MessageHandler h) {
    Slot& s = slot();
    std::lock_guard<std::mutex> lk(s.handlers_mu);
    if (h) s.on_message[type] = std::move(h);
    else s.on_message.erase(type);
}

void on_member(MemberHandler h) {
    Slot& s = slot();
    std::lock_guard<std::mutex> lk(s.handlers_mu);
    s.on_member = std::move(h);
}

bool send(int node, uint32_t type, const void* data, size_t size) {
    if (size > kMaxMessage) return false;
    const auto* b = static_cast<const uint8_t*>(data);
    return send(node, type, std::vector<uint8_t>(b, b + size));
}

bool send(int node, uint32_t type, std::vector<uint8_t>&& data) {
    if (data.size() > kMaxMessage) return false;
    const auto c = current();
    if (!c) return false;
    auto payload = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    if (!c->send(node, type, payload)) return false;
#if NXRT_TESTTOOLS
    tt::coop::on_sent(node, type, payload->data(), payload->size());
#endif
    return true;
}

int send_all(uint32_t type, const void* data, size_t size) {
    if (size > kMaxMessage) return 0;
    const auto c = current();
    if (!c) return 0;
    const auto* b = static_cast<const uint8_t*>(data);
    const int n = c->send_all(type, std::make_shared<const std::vector<uint8_t>>(b, b + size));
#if NXRT_TESTTOOLS
    if (n > 0) tt::coop::on_sent(kNoNode, type, data, size);
#endif
    return n;
}

size_t queued_bytes(int node) {
    const auto c = current();
    return c ? c->queued_bytes(node) : 0;
}

size_t drain(size_t max) {
    const auto c = current();
    if (!c) return 0;
    std::vector<vnet::CoopChannel::Item> items = c->take(max);
    Slot& s = slot();
    for (const auto& it : items) {
        if (it.kind == vnet::CoopChannel::Item::kMessage) {
#if NXRT_TESTTOOLS
            tt::coop::on_received(it.node, it.node == c->self(), it.type, it.data->data(), it.data->size());
#endif
            dispatch(it.node, it.type, it.data->data(), it.data->size());
        } else {
            MemberHandler h;
            {
                std::lock_guard<std::mutex> lk(s.handlers_mu);
                h = s.on_member;
            }
            if (h) h(MemberEvent{it.node, it.kind == vnet::CoopChannel::Item::kJoin, it.authority});
        }
    }
#if NXRT_TESTTOOLS
    if (c->self() != kNoNode) return items.size() + tt::coop::drain_injected(&dispatch);   // in a session: the handlers can act on them
    return items.size();
#else
    return items.size();
#endif
}

int self() {
    const auto c = current();
    return c ? c->self() : kNoNode;
}

int authority() {
    const auto c = current();
    return c ? c->authority() : kNoNode;
}

bool is_authority() {
    const auto c = current();
    if (!c) return false;
    const int self = c->self();
    return self != kNoNode && self == c->authority();
}

std::vector<int> members() {
    const auto c = current();
    return c ? c->members() : std::vector<int>{};
}

}  // namespace nxrt::coop_channel
