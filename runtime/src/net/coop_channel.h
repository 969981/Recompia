// SPDX-License-Identifier: GPL-3.0-or-later
// The co-op channel's engine (protocol and design: nxrt/coop_channel.h): membership, one CoopStream per member and the pump thread that
// does every port call. Game-independent and backend-independent: the session's port comes from the PortOpener it is given
// (services/ldn_channel.cpp passes the ldn backend's). Every public member is thread-safe.
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "coop_stream.h"
#include "session_port.h"

namespace nxrt::vnet {

class CoopChannel {
public:
    using Payload = CoopStream::Payload;
    // One thing for drain, in arrival order.
    struct Item {
        enum Kind : uint8_t { kJoin, kLeave, kMessage } kind = kMessage;
        int node = -1;
        bool authority = false;   // join / leave: the node is the session's authority
        uint32_t type = 0;        // message
        Payload data;             // message
    };

    // Starts the pump thread; it opens a port with opener whenever this process is in a session and has none.
    explicit CoopChannel(PortOpener opener);
    // Says goodbye to the members, closes the port and stops the thread; returns when the port is closed. Sends fail from then on.
    // Idempotent; not from the pump thread (a handler never runs there).
    void stop();
    ~CoopChannel();   // stop()
    CoopChannel(const CoopChannel&) = delete;
    CoopChannel& operator=(const CoopChannel&) = delete;

    // Queues a message to node (our own: straight to the drain queue). False when node is neither us nor a joined member.
    bool send(int node, uint32_t type, Payload data);
    int send_all(uint32_t type, const Payload& data);
    size_t queued_bytes(int node) const;
    // Removes and returns at most max items, oldest first.
    std::vector<Item> take(size_t max);

    int self() const;
    int authority() const;
    std::vector<int> members() const;

private:
    struct Peer {
        uint32_t conn = 0;   // its connection id
        bool joined = false;
        int64_t last_rx_ms = 0, last_tx_ms = 0, last_hello_ms = -1000000;
        CoopStream stream;
    };
    struct Pending {
        int node = -1;
        uint32_t type = 0;
        Payload data;
    };

    void run();
    bool open_port();
    void session_down(const char* why);
    void on_frame(uint32_t src, const uint8_t* p, size_t n, int64_t now);
    // The member's connection is over: a leave if it had joined, and its id is refused from now on (tell: send it a goodbye).
    void drop_peer(int node, const char* why, bool tell, int64_t now);
    // The member's state goes (its stream, its queued messages) and a leave is reported if it had joined; its id is not refused, so
    // its next frame finds it again as a new member.
    void forget_peer(int node, const char* why);
    // After losing a member while this process stays in the session (#827): a new connection id for this process. The member that
    // dropped us refuses our old id, and we refuse its old one; under the new id the two meet again on the next HELLO. Every other
    // member sees the new id as a new connection and starts its stream to us afresh, so ours to each of them starts afresh too.
    void renew_conn(int64_t now);
    bool tick(int64_t now);   // timers, queued messages, acknowledgements; true while a stream has work
    void send_frame(uint32_t dst, uint8_t kind, uint32_t peer_conn, const uint8_t* body, size_t n, int64_t now);
    void send_hello(int node, Peer& pe, int64_t now);
    uint32_t ip_of(int node) const { return (my_ip_ & 0xFFFFFF00u) | static_cast<uint32_t>(node + 1); }
    void push(Item&& it);   // to the drain queue (takes mu_)

    PortOpener opener_;
    // owned by the pump thread
    std::unique_ptr<SessionPort> port_;
    uint32_t my_ip_ = 0, host_ip_ = 0, my_conn_ = 0;
    std::map<int, Peer> peers_;   // by node
    struct Gone {
        uint32_t conn = 0;
        int64_t bye_ms = 0;
    };
    std::map<int, Gone> gone_;    // by node: the last connection dropped there, until a new one shows up
    int64_t last_bcast_ms_ = 0;
    size_t rr_ = 0;               // which peer pumps first (fair share of the per-tick budget)
    std::vector<uint8_t> tx_;
    std::mt19937 rng_{std::random_device{}()};   // connection ids
    // shared with the API (mu_)
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    int self_ = -1, authority_ = -1;
    std::set<int> joined_;
    std::deque<Pending> outbox_;
    std::map<int, size_t> outbox_bytes_, stream_bytes_;
    std::deque<Item> inbox_;
    std::mutex stop_mu_;   // one joiner
    std::thread thread_;
};

}  // namespace nxrt::vnet
