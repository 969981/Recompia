// SPDX-License-Identifier: GPL-3.0-or-later
// The virtual LAN the bsd HLE's sockets run on (multiplayer phase 1). A Link moves whole UDP datagrams between virtual IPs
// (169.254.R.N/24, the addresses ldn hands out); it carries no TCP and knows no ports beyond the two fields of a Datagram.
//
//   Link            one node's attachment: send() a datagram out, inbound datagrams go to the receiver set on it (any thread).
//   LoopbackHub     an in-process switch: N links, unicast by dst_ip, x.y.z.255 copied to every other link of the same /24
//                   (never back to the sender). Unit tests, and two virtual nodes in one process.
//   current link    the process-wide link the bsd socket table uses (vsock::stack()). ldn sets it on connect and clears it on
//                   disconnect (phase 2); no link = the sockets answer ENETUNREACH, as a console with no network up does.
//
// Phase 2 adds a relay client (one TCP connection to the server) implementing Link; its reader thread calls deliver().
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nxrt::vnet {

constexpr size_t kMaxDatagram = 4096;   // payload cap (the game checks 0xffd before it sends)

// One UDP datagram. IPs and ports in host byte order.
struct Datagram {
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint32_t dst_ip = 0;
    uint16_t dst_port = 0;
    std::vector<uint8_t> data;
};

using Receiver = std::function<void(const Datagram&)>;

// Thread-safe. The receiver runs on the thread that delivers (the sender's thread for LoopbackHub, a reader thread for a relay);
// it must not call back into the same link's set_receiver.
class Link {
public:
    virtual ~Link() = default;
    // The node's virtual IP, 0 when the link is down.
    virtual uint32_t local_ip() const = 0;
    // Sends one datagram; the link stamps src_ip with local_ip(). False when the link is down (nothing was sent).
    virtual bool send(const Datagram& d) = 0;
    // Where inbound datagrams go (null = dropped). Replacing it waits for a delivery in progress to finish.
    void set_receiver(Receiver r);

protected:
    void deliver(const Datagram& d);   // for implementations: hands an inbound datagram to the receiver

private:
    std::mutex rx_mu_;
    Receiver rx_;
};

class HubLink;

// In-process switch. Links stay valid after the hub object goes away (they share its state).
class LoopbackHub {
public:
    LoopbackHub();
    // A new link with this virtual IP (host order).
    std::shared_ptr<Link> add(uint32_t ip);

private:
    friend class HubLink;
    struct State;
    std::shared_ptr<State> st_;
};

// ---- the process-wide current link ----------------------------------------------------------------------------------------------------
// Setting a link wires it to the process inbound receiver; replacing or clearing it unwires the old one.
void set_current_link(std::shared_ptr<Link> link);
std::shared_ptr<Link> current_link();
// The receiver of the current link (vsock::stack() installs it once).
void set_inbound(Receiver r);

// ---- helpers --------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t{a} << 24) | (uint32_t{b} << 16) | (uint32_t{c} << 8) | d;
}
constexpr bool is_link_local(uint32_t a) { return (a >> 16) == 0xA9FEu; }          // 169.254.0.0/16
constexpr bool is_subnet_broadcast(uint32_t a) { return (a & 0xFFu) == 0xFFu; }    // x.y.z.255 of a /24
constexpr bool same_subnet(uint32_t a, uint32_t b) { return (a >> 8) == (b >> 8); } // the same /24
std::string ip_str(uint32_t a);

}  // namespace nxrt::vnet
