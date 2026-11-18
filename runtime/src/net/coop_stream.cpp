// SPDX-License-Identifier: GPL-3.0-or-later
// One reliable, ordered stream of the co-op channel (see coop_stream.h).
#include "coop_stream.h"

#include <algorithm>

#include "coop_channel_wire.h"
#include "nxrt/coop_channel.h"

namespace nxrt::vnet {

using namespace coop_wire;

void CoopStream::queue(uint32_t type, Payload data) {
    queued_bytes_ += data->size();
    out_.push_back({type, std::move(data), 0, false});
}

int CoopStream::rto_ms(int tries) const {
    int base = srtt_ms_ < 0 ? 300 : 2 * srtt_ms_ + 20;
    base = std::clamp(base, 100, 2000);
    for (int i = 1; i < tries && base < 2000; ++i) base *= 2;
    return std::min(base, 2000);
}

size_t CoopStream::pump(int64_t now_ms, size_t budget, const Emit& emit) {
    size_t sent = 0;
    const int fast_ms = std::max(20, srtt_ms_ < 0 ? 100 : srtt_ms_);
    for (auto& [seq, f] : flight_) {
        if (sent >= budget) return sent;
        if (f.sacked) continue;
        const int64_t age = now_ms - f.sent_ms;
        const bool hole = f.sent_no < arrived_no_ && age >= fast_ms;   // a frame sent after this copy arrived, this one did not
        if (!hole && age < rto_ms(f.tries)) continue;
        put_ack(f.body.data() + kDataAck);
        emit(f.body.data(), f.body.size());
        f.sent_ms = now_ms;
        f.sent_no = ++sends_;
        ++f.tries;
        ++sent;
    }
    while (sent < budget && !out_.empty() && next_seq_ - una_ < kWindow) {
        Out& o = out_.front();
        const size_t total = o.data->size();
        const size_t take = std::min(kFragmentMax, total - o.off);
        Flight f;
        const size_t head = kDataHead + (o.started ? 0 : kFirstHead);
        f.body.resize(head + take);
        uint8_t* b = f.body.data();
        put32(b, next_seq_);
        put_ack(b + kDataAck);
        b[kDataFlags] = o.started ? 0 : kFlagFirst;
        if (!o.started) {
            put32(b + kDataHead, o.type);
            put64(b + kDataHead + 4, total);
        }
        if (take) std::copy_n(o.data->data() + o.off, take, b + head);
        f.payload = take;
        f.sent_ms = now_ms;
        f.sent_no = ++sends_;
        f.tries = 1;
        o.off += take;
        o.started = true;
        emit(f.body.data(), f.body.size());
        flight_.emplace(next_seq_++, std::move(f));
        ++sent;
        if (o.off == total) out_.pop_front();
    }
    if (sent) ack_due_ = false;   // every DATA carries the ack
    return sent;
}

void CoopStream::put_ack(uint8_t* block) const {
    uint32_t sack = 0;
    for (const auto& [seq, body] : early_)
        if (seq - rx_next_ - 1 < 32) sack |= 1u << (seq - rx_next_ - 1);
    put32(block, rx_next_);
    put32(block + 4, sack);
}

void CoopStream::on_ack(const uint8_t* block, int64_t now_ms) {
    const uint32_t ack = get32(block), sack = get32(block + 4);
    if (ack < una_ || ack > next_seq_) return;   // old, or for frames never sent
    for (uint32_t i = 0; i < 32; ++i) {
        if (!(sack >> i & 1)) continue;
        auto it = flight_.find(ack + 1 + i);
        if (it == flight_.end() || it->second.sacked) continue;
        it->second.sacked = true;
        arrived_no_ = std::max(arrived_no_, it->second.sent_no);
    }
    if (ack == una_) return;
    for (auto it = flight_.begin(); it != flight_.end() && it->first < ack;) {
        arrived_no_ = std::max(arrived_no_, it->second.sent_no);
        if (it->second.tries == 1) {
            const int rtt = static_cast<int>(std::max<int64_t>(0, now_ms - it->second.sent_ms));
            srtt_ms_ = srtt_ms_ < 0 ? rtt : (7 * srtt_ms_ + rtt) / 8;
        }
        queued_bytes_ -= it->second.payload;
        it = flight_.erase(it);
    }
    una_ = ack;
}

bool CoopStream::on_data(const uint8_t* p, size_t n, int64_t now_ms, const Done& done) {
    if (n < kDataHead) return false;
    const uint32_t seq = get32(p);
    on_ack(p + kDataAck, now_ms);
    if (seq == 0) return false;
    ack_due_ = true;
    if (seq < rx_next_) return true;               // a copy of a frame already consumed: acknowledged again
    if (seq - rx_next_ >= kWindow) return true;    // beyond the window: the sender sends it again later
    bool bad = false;
    if (seq != rx_next_) {
        early_.emplace(seq, std::vector<uint8_t>(p, p + n));
        return true;
    }
    consume(p, n, done, &bad);
    ++rx_next_;
    for (auto it = early_.begin(); !bad && it != early_.end() && it->first == rx_next_; it = early_.erase(it)) {
        consume(it->second.data(), it->second.size(), done, &bad);
        ++rx_next_;
    }
    return !bad;
}

void CoopStream::consume(const uint8_t* p, size_t n, const Done& done, bool* bad) {
    size_t head = kDataHead;
    if (p[kDataFlags] & kFlagFirst) {
        if (assembling_ || n < kDataHead + kFirstHead) {
            *bad = true;
            return;
        }
        msg_type_ = get32(p + kDataHead);
        msg_total_ = get64(p + kDataHead + 4);
        if (msg_total_ > coop_channel::kMaxMessage) {
            *bad = true;
            return;
        }
        msg_.clear();
        msg_.reserve(static_cast<size_t>(msg_total_));
        assembling_ = true;
        head += kFirstHead;
    } else if (!assembling_) {
        *bad = true;
        return;
    }
    const size_t take = n - head;
    if (msg_.size() + take > msg_total_) {
        *bad = true;
        return;
    }
    msg_.insert(msg_.end(), p + head, p + n);
    if (msg_.size() == msg_total_) {
        assembling_ = false;
        done(msg_type_, std::move(msg_));
        msg_ = {};
    }
}

}  // namespace nxrt::vnet
