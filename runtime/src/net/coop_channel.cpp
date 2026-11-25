// SPDX-License-Identifier: GPL-3.0-or-later
// The co-op channel's engine: the pump thread, membership and the drain queue (see coop_channel.h; wire format in nxrt/coop_channel.h).
// Lock: mu_ guards what the API reads and writes (view, outbox, drain queue); everything else belongs to the pump thread, which never
// calls the port with mu_ held.
#include "coop_channel.h"

#include <algorithm>
#include <chrono>

#include "coop_channel_wire.h"
#include "nxrt/coop_channel.h"
#include "nxrt/log.h"
#include "vnet.h"

namespace nxrt::vnet {

using namespace coop_wire;

namespace {
constexpr int kHelloEveryMs = 1000;      // broadcast: members that open their channel later find us
constexpr int kKeepaliveMs = 500;        // to a joined member we have sent nothing to for this long
constexpr int kHelloReplyGapMs = 200;    // at most one answering HELLO per member this often
constexpr int kReopenWaitMs = 250;       // between tries to open a port while in no session
constexpr int kIdleWaitMs = 10, kBusyWaitMs = 2;   // receive wait per pump round
constexpr size_t kRecvPerRound = 512;
constexpr size_t kFramesPerRound = 64;  // DATA frames per pump round, all members together (paces bulk sends)

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int node_of(uint32_t ip) { return static_cast<int>(ip & 0xFFu) - 1; }
}  // namespace

CoopChannel::CoopChannel(PortOpener opener) : opener_(std::move(opener)), tx_(kMaxDatagram) {
    thread_ = std::thread([this] { run(); });
}

void CoopChannel::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    std::lock_guard<std::mutex> lk(stop_mu_);
    if (thread_.joinable()) thread_.join();
}

CoopChannel::~CoopChannel() { stop(); }

// ---- API side -----------------------------------------------------------------------------------------------------------------------

void CoopChannel::push(Item&& it) {
    std::lock_guard<std::mutex> lk(mu_);
    inbox_.push_back(std::move(it));
}

bool CoopChannel::send(int node, uint32_t type, Payload data) {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_ || self_ < 0) return false;
    if (node == self_) {
        Item it;
        it.kind = Item::kMessage;
        it.node = node;
        it.type = type;
        it.data = std::move(data);
        inbox_.push_back(std::move(it));
        return true;
    }
    if (!joined_.count(node)) return false;
    outbox_bytes_[node] += data->size();
    outbox_.push_back({node, type, std::move(data)});
    return true;
}

int CoopChannel::send_all(uint32_t type, const Payload& data) {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_ || self_ < 0) return 0;
    for (const int node : joined_) {
        outbox_bytes_[node] += data->size();
        outbox_.push_back({node, type, data});
    }
    return static_cast<int>(joined_.size());
}

size_t CoopChannel::queued_bytes(int node) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    if (auto it = outbox_bytes_.find(node); it != outbox_bytes_.end()) n += it->second;
    if (auto it = stream_bytes_.find(node); it != stream_bytes_.end()) n += it->second;
    return n;
}

std::vector<CoopChannel::Item> CoopChannel::take(size_t max) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Item> out;
    const size_t n = std::min(max, inbox_.size());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
    return out;
}

int CoopChannel::self() const {
    std::lock_guard<std::mutex> lk(mu_);
    return self_;
}

int CoopChannel::authority() const {
    std::lock_guard<std::mutex> lk(mu_);
    return authority_;
}

std::vector<int> CoopChannel::members() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {joined_.begin(), joined_.end()};
}

// ---- pump thread ----------------------------------------------------------------------------------------------------------------------

void CoopChannel::run() {
    log_set_thread_name("coop-channel");
    std::vector<uint8_t> rx(kMaxDatagram);
    bool busy = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (stop_) break;
            if (!port_) {
                lk.unlock();
                if (!open_port()) {
                    lk.lock();
                    cv_.wait_for(lk, std::chrono::milliseconds(kReopenWaitMs), [this] { return stop_; });
                    continue;
                }
            }
        }
        if (port_->local_ip() != my_ip_) {
            session_down("this process left the session");
            continue;
        }
        for (size_t i = 0; i < kRecvPerRound; ++i) {
            uint32_t src = 0;
            const int r = port_->recv(rx.data(), rx.size(), &src, i == 0 ? (busy ? kBusyWaitMs : kIdleWaitMs) : 0);
            if (r < 0) {
                session_down("the session's socket closed");
                break;
            }
            if (r == 0) break;
            on_frame(src, rx.data(), static_cast<size_t>(r), now_ms());
        }
        if (port_) busy = tick(now_ms());
    }
    if (port_) {
        const int64_t now = now_ms();
        send_frame((my_ip_ & 0xFFFFFF00u) | 0xFFu, kBye, 0, nullptr, 0, now);
        for (auto& [node, pe] : peers_) send_frame(ip_of(node), kBye, pe.conn, nullptr, 0, now);
        NXLOG_I("net", "coop channel: closed");
        port_.reset();
    }
}

bool CoopChannel::open_port() {
    std::unique_ptr<SessionPort> p = opener_(coop_channel::kPort);
    if (!p) return false;
    const uint32_t ip = p->local_ip();
    if (!ip) return false;
    uint32_t conn = 0;
    while (!conn) conn = static_cast<uint32_t>(rng_());
    port_ = std::move(p);
    my_ip_ = ip;
    host_ip_ = port_->host_ip();
    my_conn_ = conn;
    last_bcast_ms_ = 0;
    peers_.clear();
    gone_.clear();
    const int self = node_of(my_ip_), auth = node_of(host_ip_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        self_ = self;
        authority_ = auth;
        joined_.clear();
        Item it;
        it.kind = Item::kJoin;
        it.node = self;
        it.authority = self == auth;
        inbox_.push_back(std::move(it));
    }
    NXLOG_I("net", "coop channel: in the session as node %d (%s)%s, authority node %d", self, ip_str(my_ip_).c_str(),
            self == auth ? ", the authority" : "", auth);
    return true;
}

void CoopChannel::session_down(const char* why) {
    NXLOG_I("net", "coop channel: out of the session (%s): %zu member(s) leave", why, peers_.size());
    std::vector<int> nodes;
    for (auto& [node, pe] : peers_)
        if (pe.joined) nodes.push_back(node);
    peers_.clear();
    gone_.clear();
    port_.reset();
    std::lock_guard<std::mutex> lk(mu_);
    for (const int node : nodes) {
        Item it;
        it.kind = Item::kLeave;
        it.node = node;
        it.authority = node == authority_;
        inbox_.push_back(std::move(it));
    }
    Item me;
    me.kind = Item::kLeave;
    me.node = self_;
    me.authority = self_ == authority_;
    inbox_.push_back(std::move(me));
    self_ = authority_ = -1;
    joined_.clear();
    outbox_.clear();
    outbox_bytes_.clear();
    stream_bytes_.clear();
    my_ip_ = host_ip_ = my_conn_ = 0;
}

void CoopChannel::drop_peer(int node, const char* why, bool tell, int64_t now) {
    auto it = peers_.find(node);
    if (it == peers_.end()) return;
    const uint32_t conn = it->second.conn;
    gone_[node] = {conn, now};
    if (tell) send_frame(ip_of(node), kBye, conn, nullptr, 0, now);
    forget_peer(node, why);
}

void CoopChannel::forget_peer(int node, const char* why) {
    auto it = peers_.find(node);
    if (it == peers_.end()) return;
    const bool joined = it->second.joined;
    peers_.erase(it);
    NXLOG_I("net", "coop channel: node %d %s", node, why);
    if (!joined) return;
    std::lock_guard<std::mutex> lk(mu_);
    joined_.erase(node);
    outbox_bytes_.erase(node);
    stream_bytes_.erase(node);
    outbox_.erase(std::remove_if(outbox_.begin(), outbox_.end(), [node](const Pending& p) { return p.node == node; }), outbox_.end());
    Item item;
    item.kind = Item::kLeave;
    item.node = node;
    item.authority = node == authority_;
    inbox_.push_back(std::move(item));
}

// Nothing else gives a process a new id while the session lives (my_conn_ was set in open_port only), so after a gap longer than
// kPeerTimeoutMs a member and this process went on refusing each other until the session itself ended: 195 to 227 s in the logs of
// #827, with story steps finished meanwhile shared with nobody. The members still here are forgotten, not refused: a stream kept across
// the change would carry sequence numbers the member's new stream to us does not take (frames beyond its window, acks it ignores).
void CoopChannel::renew_conn(int64_t now) {
    uint32_t conn = 0;
    while (!conn || conn == my_conn_) conn = static_cast<uint32_t>(rng_());
    my_conn_ = conn;
    std::vector<int> nodes;
    for (const auto& [node, pe] : peers_) nodes.push_back(node);
    for (const int node : nodes) forget_peer(node, "meets this process again under its new connection id");
    last_bcast_ms_ = now - kHelloEveryMs;   // say HELLO on the next tick
    NXLOG_I("net", "coop channel: new connection id after losing a member; the session goes on");
}

void CoopChannel::on_frame(uint32_t src, const uint8_t* p, size_t n, int64_t now) {
    Header h;
    if (!get_header(p, n, &h) || src == my_ip_ || !same_subnet(src, my_ip_)) return;
    const int node = node_of(src);
    if (node < 0 || node >= 0xFE) return;
    if (auto g = gone_.find(node); g != gone_.end()) {
        if (g->second.conn == h.conn) {   // a connection we dropped: tell it again, in case it has not heard (our goodbye was lost)
            if (h.kind != kBye && now - g->second.bye_ms >= kHelloReplyGapMs) {
                send_frame(src, kBye, h.conn, nullptr, 0, now);
                g->second.bye_ms = now;
            }
            return;
        }
        gone_.erase(g);
    }
    auto it = peers_.find(node);
    if (h.kind == kBye) {
        if (it != peers_.end() && it->second.conn == h.conn) {
            drop_peer(node, "left (goodbye)", false, now);
            renew_conn(now);   // it may still be in the session (it timed us out): meet again under a new id
        }
        return;
    }
    if (it != peers_.end() && it->second.conn != h.conn) {
        drop_peer(node, "came back as a new connection", false, now);
        it = peers_.end();
    }
    if (it == peers_.end()) {
        it = peers_.try_emplace(node).first;
        it->second.conn = h.conn;
        NXLOG_D("net", "coop channel: node %d (%s) found", node, ip_str(src).c_str());
    }
    Peer& pe = it->second;
    pe.last_rx_ms = now;
    if (h.peer_conn != my_conn_) {   // it does not know this connection yet: say who we are, use nothing else of the frame
        const bool bcast_from_joined = pe.joined && h.peer_conn == 0;   // its periodic broadcast: nothing to answer
        if (!bcast_from_joined && now - pe.last_hello_ms >= kHelloReplyGapMs) send_hello(node, pe, now);
        return;
    }
    if (!pe.joined) {
        pe.joined = true;
        send_hello(node, pe, now);   // the other side may still be waiting for our answer
        NXLOG_I("net", "coop channel: node %d joined%s", node, node == node_of(host_ip_) ? " (the authority)" : "");
        std::lock_guard<std::mutex> lk(mu_);
        joined_.insert(node);
        Item item;
        item.kind = Item::kJoin;
        item.node = node;
        item.authority = node == authority_;
        inbox_.push_back(std::move(item));
    }
    const uint8_t* body = p + kHeader;
    const size_t len = n - kHeader;
    if (h.kind == kAck) {
        if (len >= kAckBlock) pe.stream.on_ack(body, now);
    } else if (h.kind == kData) {
        const bool ok = pe.stream.on_data(body, len, now, [this, node](uint32_t type, std::vector<uint8_t>&& data) {
            Item item;
            item.kind = Item::kMessage;
            item.node = node;
            item.type = type;
            item.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
            push(std::move(item));
        });
        if (!ok) drop_peer(node, "sent a malformed frame: dropped", true, now);
    }
}

void CoopChannel::send_frame(uint32_t dst, uint8_t kind, uint32_t peer_conn, const uint8_t* body, size_t n, int64_t now) {
    (void)now;
    put_header(tx_.data(), {static_cast<Kind>(kind), my_conn_, peer_conn});
    if (n) std::copy_n(body, n, tx_.data() + kHeader);
    port_->send(dst, tx_.data(), kHeader + n);
}

void CoopChannel::send_hello(int node, Peer& pe, int64_t now) {
    send_frame(ip_of(node), kHello, pe.conn, nullptr, 0, now);
    pe.last_hello_ms = pe.last_tx_ms = now;
}

bool CoopChannel::tick(int64_t now) {
    if (now - last_bcast_ms_ >= kHelloEveryMs) {
        send_frame((my_ip_ & 0xFFFFFF00u) | 0xFFu, kHello, 0, nullptr, 0, now);
        last_bcast_ms_ = now;
    }
    std::vector<int> dead;
    for (auto& [node, pe] : peers_) {
        if (now - pe.last_rx_ms > coop_channel::kPeerTimeoutMs) dead.push_back(node);
        else if (pe.joined && now - pe.last_tx_ms >= kKeepaliveMs) send_hello(node, pe, now);
    }
    for (const int node : dead) drop_peer(node, "timed out", true, now);
    if (!dead.empty()) renew_conn(now);   // the session lives on: a member that comes back meets a new id, not a refused one

    std::deque<Pending> fresh;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fresh.swap(outbox_);
        outbox_bytes_.clear();
    }
    for (Pending& m : fresh) {
        auto it = peers_.find(m.node);
        if (it != peers_.end() && it->second.joined) it->second.stream.queue(m.type, std::move(m.data));
    }

    size_t budget = kFramesPerRound;
    bool busy = false;
    if (!peers_.empty()) {
        std::vector<std::pair<const int, Peer>*> order;
        for (auto& e : peers_) order.push_back(&e);
        rr_ = (rr_ + 1) % order.size();
        std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(rr_), order.end());
        for (auto* e : order) {
            const int node = e->first;
            Peer& pe = e->second;
            if (!pe.joined) continue;
            const uint32_t dst = ip_of(node);
            const size_t sent = pe.stream.pump(now, budget, [&](const uint8_t* body, size_t n) {
                send_frame(dst, kData, pe.conn, body, n, now);
            });
            if (sent) pe.last_tx_ms = now;
            budget -= sent;
            if (pe.stream.ack_due()) {
                uint8_t ack[kAckBlock];
                pe.stream.put_ack(ack);
                send_frame(dst, kAck, pe.conn, ack, sizeof ack, now);
                pe.stream.ack_sent();
                pe.last_tx_ms = now;
            }
            busy = busy || pe.stream.busy();
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    stream_bytes_.clear();
    for (auto& [node, pe] : peers_)
        if (pe.stream.queued_bytes()) stream_bytes_[node] = pe.stream.queued_bytes();
    return busy;
}

}  // namespace nxrt::vnet
