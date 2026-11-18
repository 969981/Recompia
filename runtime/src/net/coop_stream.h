// SPDX-License-Identifier: GPL-3.0-or-later
// One reliable, ordered message stream between this process and one member of the co-op channel (one connection; nxrt/coop_channel.h has
// the wire format). Pure protocol state: no sockets, no threads, no locks; the pump thread (coop_channel.cpp) owns it, feeds it the DATA and
// ACK bodies that arrive and sends the DATA bodies it produces.
//   Sending    queue() takes whole messages; pump() cuts them into fragments as the window opens (kWindow frames unacknowledged) and
//              sends a frame again when its timer runs out (2 x the smoothed round trip + 20 ms, 100 ms .. 2 s, doubled per retry, at
//              most 2 s), or sooner when a frame sent after it has arrived (acknowledged or reported kept early in sack) and it has
//              been out for one smoothed round trip (at least 20 ms, the reordering allowance); that rule repeats for a resent frame
//              lost again, so one loss costs about a round trip, not a timer. Frames reported kept early are not sent again.
//              Retransmits carry the current acknowledgement.
//   Receiving  on_data() keeps frames up to kWindow ahead of the next expected one, consumes them in order, rebuilds messages and hands
//              each complete one to done. A frame already consumed is only acknowledged again.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace nxrt::vnet {

class CoopStream {
public:
    using Payload = std::shared_ptr<const std::vector<uint8_t>>;
    using Emit = std::function<void(const uint8_t* body, size_t n)>;
    using Done = std::function<void(uint32_t type, std::vector<uint8_t>&& data)>;

    void queue(uint32_t type, Payload data);
    // Produces DATA bodies to send now, retransmits first, at most budget of them. Returns how many went to emit.
    size_t pump(int64_t now_ms, size_t budget, const Emit& emit);
    // A DATA body from the member. False: malformed (the caller drops the connection).
    bool on_data(const uint8_t* p, size_t n, int64_t now_ms, const Done& done);
    // An acknowledgement block (coop_wire::kAckBlock bytes) from the member.
    void on_ack(const uint8_t* block, int64_t now_ms);

    // A frame arrived since the last acknowledgement went out (in a DATA or an ACK).
    bool ack_due() const { return ack_due_; }
    void put_ack(uint8_t* block) const;   // writes our acknowledgement block
    void ack_sent() { ack_due_ = false; }

    size_t queued_bytes() const { return queued_bytes_; }   // payload not yet acknowledged
    bool busy() const { return !out_.empty() || !flight_.empty(); }

private:
    struct Out {
        uint32_t type = 0;
        Payload data;
        size_t off = 0;
        bool started = false;
    };
    struct Flight {
        std::vector<uint8_t> body;
        size_t payload = 0;
        int64_t sent_ms = 0;
        int tries = 0;
        uint64_t sent_no = 0;   // send order of its latest copy (every frame sent counts one)
        bool sacked = false;    // the receiver keeps it early: never sent again
    };
    int rto_ms(int tries) const;
    void consume(const uint8_t* p, size_t n, const Done& done, bool* bad);

    // sending
    std::deque<Out> out_;
    std::map<uint32_t, Flight> flight_;   // by seq
    uint32_t next_seq_ = 1;
    uint32_t una_ = 1;                    // lowest seq not acknowledged
    uint64_t sends_ = 0;                  // frames sent so far (send order)
    uint64_t arrived_no_ = 0;             // the latest send order known to have arrived
    int srtt_ms_ = -1;
    size_t queued_bytes_ = 0;
    // receiving
    uint32_t rx_next_ = 1;
    std::map<uint32_t, std::vector<uint8_t>> early_;   // seq -> body, ahead of rx_next_
    bool ack_due_ = false;
    bool assembling_ = false;
    uint32_t msg_type_ = 0;
    uint64_t msg_total_ = 0;
    std::vector<uint8_t> msg_;
};

}  // namespace nxrt::vnet
