// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt/coop_channel.h: the co-op channel's entry points. Holds the process's one channel (net/coop_channel.h) on the ports of the ldn
// backend [net] protocol picks, the handlers the game layer registered, and drain(). ldn_reset() closes it before the backends reset.
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "ldn_backend.h"
#include "nxrt/coop_channel.h"
#include "nxrt/log.h"
#include "../net/coop_channel.h"

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
    return c && c->send(node, type, std::make_shared<const std::vector<uint8_t>>(std::move(data)));
}

int send_all(uint32_t type, const void* data, size_t size) {
    if (size > kMaxMessage) return 0;
    const auto c = current();
    if (!c) return 0;
    const auto* b = static_cast<const uint8_t*>(data);
    return c->send_all(type, std::make_shared<const std::vector<uint8_t>>(b, b + size));
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
            MessageHandler h;
            {
                std::lock_guard<std::mutex> lk(s.handlers_mu);
                if (auto f = s.on_message.find(it.type); f != s.on_message.end()) h = f->second;
                else if (s.unhandled_logged.insert(it.type).second)
                    NXLOG_W("net", "coop channel: no handler for message type %u (from node %d): dropped", it.type, it.node);
            }
            if (h) h(Message{it.node, it.type, it.data->data(), it.data->size()});
        } else {
            MemberHandler h;
            {
                std::lock_guard<std::mutex> lk(s.handlers_mu);
                h = s.on_member;
            }
            if (h) h(MemberEvent{it.node, it.kind == vnet::CoopChannel::Item::kJoin, it.authority});
        }
    }
    return items.size();
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
