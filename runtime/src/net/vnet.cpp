// SPDX-License-Identifier: GPL-3.0-or-later
// The virtual LAN: Link receiver plumbing, the in-process LoopbackHub and the process-wide current link (see vnet.h).
// Lock order: the current-link lock, then a link's receiver lock, then whatever the receiver takes (the socket table's lock).
#include "vnet.h"

#include <cstdio>

namespace nxrt::vnet {

void Link::set_receiver(Receiver r) {
    std::lock_guard<std::mutex> lk(rx_mu_);
    rx_ = std::move(r);
}

void Link::deliver(const Datagram& d) {
    std::lock_guard<std::mutex> lk(rx_mu_);   // held across the call: set_receiver(nullptr) returning means no call is running
    if (rx_) rx_(d);
}

// ---- LoopbackHub ----------------------------------------------------------------------------------------------------------------------

struct LoopbackHub::State {
    std::mutex mu;
    std::vector<std::weak_ptr<HubLink>> links;
};

class HubLink final : public Link {
public:
    HubLink(std::shared_ptr<LoopbackHub::State> st, uint32_t ip) : st_(std::move(st)), ip_(ip) {}
    uint32_t local_ip() const override { return ip_; }
    bool send(const Datagram& in) override {
        Datagram d = in;
        d.src_ip = ip_;                       // like the relay: the sender cannot pick its source address
        std::vector<std::shared_ptr<HubLink>> to;
        {
            std::lock_guard<std::mutex> lk(st_->mu);
            for (const auto& w : st_->links) {
                auto l = w.lock();
                if (!l) continue;
                const bool bcast = is_subnet_broadcast(d.dst_ip) && same_subnet(d.dst_ip, ip_) && l.get() != this &&
                                   same_subnet(l->ip_, ip_);
                if (bcast || (!is_subnet_broadcast(d.dst_ip) && l->ip_ == d.dst_ip)) to.push_back(std::move(l));
            }
        }
        for (const auto& l : to) l->deliver(d);   // outside the hub lock: a receiver may send in turn
        return true;
    }

private:
    std::shared_ptr<LoopbackHub::State> st_;
    uint32_t ip_;
};

LoopbackHub::LoopbackHub() : st_(std::make_shared<State>()) {}

std::shared_ptr<Link> LoopbackHub::add(uint32_t ip) {
    auto l = std::make_shared<HubLink>(st_, ip);
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->links.push_back(l);
    return l;
}

// ---- current link ---------------------------------------------------------------------------------------------------------------------

namespace {
struct Current {
    std::mutex mu;
    std::shared_ptr<Link> link;
    Receiver inbound;
};
Current& cur() {
    static Current c;
    return c;
}
}  // namespace

void set_current_link(std::shared_ptr<Link> link) {
    Current& c = cur();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.link == link) return;
    if (c.link) c.link->set_receiver(nullptr);
    c.link = std::move(link);
    if (c.link) c.link->set_receiver(c.inbound);
}

std::shared_ptr<Link> current_link() {
    Current& c = cur();
    std::lock_guard<std::mutex> lk(c.mu);
    return c.link;
}

void set_inbound(Receiver r) {
    Current& c = cur();
    std::lock_guard<std::mutex> lk(c.mu);
    c.inbound = std::move(r);
    if (c.link) c.link->set_receiver(c.inbound);
}

std::string ip_str(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    return b;
}

}  // namespace nxrt::vnet
