// SPDX-License-Identifier: GPL-3.0-or-later
// The co-op channel: reliable, ordered messages between the processes of one Link Play session, for a game layer that runs its own rules
// next to the game's replication. Game-independent: payloads are bytes under a type id the game layer registers; the channel knows nothing
// of what they mean. Implemented by net/coop_channel.cpp (protocol, pump thread), net/coop_stream.cpp (one reliable stream per peer) and
// services/ldn_channel.cpp (this API, bound to the ldn backend in use: ldn_native_port.cpp or ldn_linkplay_port.cpp).
//
// Design
//   Transport   UDP datagrams on the session's virtual network (169.254.R.0/24), virtual port kPort, through the ldn backend in use
//               ([net] protocol native: the co-op module's virtual sockets; linkplay: the relay room's socket table). Local (several
//               processes on one PC), LAN and relay sessions are the same to the channel. Datagrams are at most 4096 bytes and may be lost,
//               duplicated or reordered (UDP legs), so the channel adds its own reliability on top. Nothing is opened until open(); a
//               build without the channel never answers on kPort, so a member with no channel is never reported.
//   Members     A member is a process of the session whose channel is open. Its id is its node: the last byte of its virtual address
//               minus 1 (0..7). The authority is the process that opened the session (node 0, the network's host). Every process
//               talks to every other directly (a mesh of streams; the backends route the datagrams through their own hub). Our own
//               node gets a join event first when the channel is in a session (authority set when we are it) and a leave last when it
//               is out of it, so the game layer learns from the events alone when it can send and who the authority is.
//   Wire        Little-endian. Every datagram: header (16 bytes) = magic "NXCC" (4), version (1), kind (1), 2 zero bytes,
//               sender's connection id (4), receiver's connection id as the sender knows it (4; 0 = unknown). A connection id is random
//               per process and session, so a member that leaves and comes back on the same address is a new connection: stale frames
//               of the old one are dropped. Kinds:
//                 HELLO  no body. Broadcast every second, sent to one member to answer a HELLO that does not know us yet and as a
//                        keepalive. A frame that names our connection id proves the other side knows us: the member has joined.
//                 DATA   seq (4), ack (4), sack (4), flags (1: bit 0 first fragment of a message); a first fragment then has type
//                        (4) and total size (8); then fragment bytes (every fragment the same size bar the last). seq counts from 1
//                        per connection; ack is the next seq the sender expects from the receiver (cumulative); sack bit i: seq
//                        ack + 1 + i arrived early and is kept.
//                 ACK    ack (4), sack (4).
//                 BYE    no body: the sender has closed its channel, or has dropped our connection; we drop it at once.
//   Ordering    One stream per pair of members: frames carry a sequence number, the receiver keeps up to 32 frames that arrive
//               early (reported back in sack, so the sender resends only the holes) and delivers in order; duplicates are
//               acknowledged and dropped. Messages from one sender arrive in the order sent, exactly once, while its connection
//               lives. There is no order between different senders.
//   Chunking    A message of any size up to kMaxMessage is cut into fragments when the pump sends them, so a large payload (a save
//               file) costs the game thread one copy (or none: send() with a moved vector), and it streams at 32 frames in flight
//               per member while smaller messages queued behind it wait their turn on that stream. The receiver builds it up and
//               delivers it whole.
//   Threads     send() queues and returns. One pump thread per process does every socket call: it receives, acknowledges,
//               resends what was lost (once a frame sent after it is known to have arrived, else on a timer from the measured
//               round trip, 100 ms .. 2 s) and keeps at most 32 frames unacknowledged per member (kWindow in
//               net/coop_channel_wire.h). Received messages and member events wait in one queue until the game layer calls drain()
//               on the thread it wants them on (once per frame, say); handlers run there, in arrival order. A join is always
//               drained before its member's first message, a leave after its last.
//   Failure     A member silent for kPeerTimeoutMs (keepalives every 500 ms), a BYE, a new connection id on its address or a
//               malformed frame from it is reported as a leave; what it had in flight is dropped both ways. A member we drop is sent a
//               BYE, and its connection id is refused (answered with BYE) until it comes back with a new one. A process that loses a
//               member while it stays in the session takes a new connection id itself (the member refuses its old one too) and meets
//               every member again under it: a gap longer than kPeerTimeoutMs is a leave and a join, not a split. When this process leaves
//               the session (its virtual address goes away or the backend's socket dies) every member leaves, and the channel joins
//               the next session on its own while it stays open. Messages to a member that has not joined (or left) are refused.
//   Loopback    A message to our own node (the authority sending to the authority) goes through the same queue and drain() as every
//               other message, never inline from send(), so a rule runs the same way whoever sent it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace nxrt::coop_channel {

constexpr uint16_t kPort = 48620;                  // virtual port on the session network; below the ephemeral range (49152)
constexpr size_t kMaxMessage = size_t{256} << 20;   // largest payload one send() takes
constexpr int kPeerTimeoutMs = 6000;               // silence after which a member has left
constexpr int kNoNode = -1;

struct Message {
    int from = kNoNode;   // sender's node (our own for a loopback message)
    uint32_t type = 0;
    const uint8_t* data = nullptr;   // valid during the handler call only
    size_t size = 0;
};
using MessageHandler = std::function<void(const Message&)>;

struct MemberEvent {
    int node = kNoNode;
    bool joined = false;      // false: left
    bool authority = false;   // the member is the session's authority
};
using MemberHandler = std::function<void(const MemberEvent&)>;

// Starts the channel (idempotent; true once open): from now on it joins every session this process is in, through the ldn backend
// [net] protocol picks (with local wireless off it waits for a session that never comes). Call from any thread.
bool open();
// Says goodbye to every member and stops; returns when the port is closed. Queued messages are dropped; handlers stay registered.
// ldn_reset() also closes it.
void close();
bool is_open();

// Handlers, by type (any thread; null removes). A type with no handler: its messages are dropped at drain (logged once per type).
void on_message(uint32_t type, MessageHandler h);
void on_member(MemberHandler h);

// Queues one message. node may be our own (loopback). False: closed, not in a session, the node has not joined, or too large.
bool send(int node, uint32_t type, const void* data, size_t size);
bool send(int node, uint32_t type, std::vector<uint8_t>&& data);
// Queues one message to every member that has joined, not to this process. Returns how many it was queued for.
int send_all(uint32_t type, const void* data, size_t size);
// Payload bytes queued or in flight to node (0 when none or unknown): a sender of bulk data can pace itself or show progress.
size_t queued_bytes(int node);

// Runs the handlers of everything received since the last call, in order, on the calling thread; at most max items. Returns how many.
size_t drain(size_t max = SIZE_MAX);

// This process's node and the authority's (kNoNode outside a session), whether we are it, and the members that have joined (not us).
int self();
int authority();
bool is_authority();
std::vector<int> members();

}  // namespace nxrt::coop_channel
