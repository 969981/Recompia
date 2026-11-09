// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u on pokopia-native's co-op module ([net] protocol = native, the default): the process's coop node and NativeLdn (ldn_native.cpp).
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "coop.h"
#include "ldn_backend.h"
#include "ldn_internal.h"

namespace nxrt::services::ldn_impl {

bool native_protocol();

struct NativeSlot {
    std::mutex mu;
    bool built = false;
    std::atomic<coop_node*> node{nullptr};
    ServiceEvent ev;   // AttachStateChangeEvent of every service object; signalled by the module's on_state
    std::string build;
};
NativeSlot& native();
// The process's coop node from [net] (made once; null: local wireless off).
coop_node* native_node();

class NativeLdn final : public LdnSession, public std::enable_shared_from_this<NativeLdn> {
public:
    explicit NativeLdn(coop_node* n) : n_(n) {}
    void add_commands(ipc::Service& s);
    void shutdown() override {
        coop_ldn_finalize(n_);
        forget();
    }

private:
    using H = uint32_t (NativeLdn::*)(ipc::Request&, ipc::Response&);
    void cmd(ipc::Service& s, uint32_t id, const char* name, H h);
    void forget() {
        std::lock_guard<std::mutex> lk(mu_);
        seen_ = {};
        latest_ = {};
    }
    // The game read `ni`: node joins and leaves since the last table it read go into latest_ (GetNetworkInfoLatestUpdate).
    void saw(const NetworkInfo& ni) {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < 8; ++i) {
            const uint32_t now = ni.ldn.nodes[i].is_connected ? ni.ldn.nodes[i].ipv4 : 0;
            if (now == seen_[i]) continue;
            if (seen_[i]) latest_[i] |= kNodeDisconnect;
            if (now) latest_[i] |= kNodeConnect;
            seen_[i] = now;
        }
    }
    // A command that may have left the network: the node table the game saw is gone.
    uint32_t left(uint32_t r) {
        const int st = coop_ldn_state(n_);
        if (st != COOP_LS_AP_CREATED && st != COOP_LS_STATION_CONNECTED) forget();
        return r;
    }

    uint32_t get_state(ipc::Request&, ipc::Response& rsp) {
        rsp.push(static_cast<uint32_t>(coop_ldn_state(n_)));
        return 0;
    }
    uint32_t get_network_info(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        if (const uint32_t r = coop_ldn_network_info(n_, &ni)) return r;
        buf_write(req.out_buf(0), &ni, sizeof ni);
        saw(ni);
        return 0;
    }
    uint32_t get_ipv4(ipc::Request&, ipc::Response& rsp) {
        uint32_t ip = 0, mask = 0;
        if (const uint32_t r = coop_ldn_ipv4(n_, &ip, &mask)) return r;
        rsp.push(ip);
        rsp.push(mask);
        return 0;
    }
    uint32_t get_disconnect_reason(ipc::Request&, ipc::Response& rsp) {
        rsp.push(static_cast<int16_t>(coop_ldn_disconnect_reason(n_)));
        return 0;
    }
    uint32_t get_security_parameter(ipc::Request&, ipc::Response& rsp) {
        SecurityParameter sp{};
        if (const uint32_t r = coop_ldn_security_parameter(n_, &sp)) return r;
        rsp.push(sp);
        return 0;
    }
    uint32_t get_network_config(ipc::Request&, ipc::Response& rsp) {
        NetworkConfig c{};
        if (const uint32_t r = coop_ldn_network_config(n_, &c)) return r;
        rsp.push(c);
        return 0;
    }
    uint32_t attach_event(ipc::Request&, ipc::Response& rsp) {
        rsp.out_copy.push_back(native().ev.new_handle());
        return 0;
    }
    uint32_t get_latest_update(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        if (const uint32_t r = coop_ldn_network_info(n_, &ni)) return r;
        buf_write(req.out_buf(0), &ni, sizeof ni);
        saw(ni);
        NodeLatestUpdate up[8] = {};
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < 8; ++i) up[i].state_change = latest_[i];
            latest_ = {};
        }
        buf_write(req.out_buf(1), up, sizeof up);
        return 0;
    }
    uint32_t scan(ipc::Request& req, ipc::Response& rsp) {
        const int channel = req.arg<int16_t>(0);
        const ScanFilter f = arg_at<ScanFilter>(req, 8);   // after the s16 channel, 8-aligned
        const ipc::Buffer out = req.out_buf(0);
        const int cap = static_cast<int>(std::min<uint64_t>(out.size / sizeof(NetworkInfo), 64));
        std::vector<NetworkInfo> found(static_cast<size_t>(cap > 0 ? cap : 1));
        int count = 0;
        if (const uint32_t r = coop_ldn_scan(n_, found.data(), &count, cap, &f, channel)) return r;
        if (count > 0) buf_write(out, found.data(), sizeof(NetworkInfo) * static_cast<size_t>(count));
        rsp.push(static_cast<uint32_t>(count));   // s16 total (the high half is zero)
        return 0;
    }
    uint32_t accept(ipc::Request&, ipc::Response&) { return 0; }
    uint32_t open_access_point(ipc::Request&, ipc::Response&) { return coop_ldn_open_ap(n_); }
    uint32_t close_access_point(ipc::Request&, ipc::Response&) { return left(coop_ldn_close_ap(n_)); }
    uint32_t create_network(ipc::Request& req, ipc::Response&) {
        const SecurityConfig sec = arg_at<SecurityConfig>(req, 0);
        const UserConfig user = arg_at<UserConfig>(req, kCreateUser);
        const NetworkConfig cfg = arg_at<NetworkConfig>(req, kCreateNetCfg);
        forget();
        return coop_ldn_create_network(n_, &cfg, &sec, &user);
    }
    uint32_t create_network_private(ipc::Request&, ipc::Response&) {
        NXLOG_W("net", "ldn: CreateNetworkPrivate has no co-op counterpart: DeviceNotAvailable");
        return kDeviceNotAvailable;
    }
    uint32_t destroy_network(ipc::Request&, ipc::Response&) { return left(coop_ldn_destroy_network(n_)); }
    uint32_t reject(ipc::Request& req, ipc::Response&) { return coop_ldn_reject(n_, req.arg<uint32_t>(0)); }
    uint32_t set_advertise_data(ipc::Request& req, ipc::Response&) {
        const ipc::Buffer b = req.in_buf(0);
        if (b.size > vnet::kMaxAdvertise) return kInvalidArgument;
        std::vector<uint8_t> adv(buf_ptr(b), buf_ptr(b) + (b.addr ? b.size : 0));
        NXLOG_D("net", "ldn: SetAdvertiseData %zu bytes: %s", adv.size(), hex_head(adv).c_str());
        return coop_ldn_set_advertise(n_, adv.data(), adv.size());
    }
    uint32_t set_accept_policy(ipc::Request& req, ipc::Response&) { return coop_ldn_set_accept_policy(n_, req.arg<uint8_t>(0)); }
    uint32_t accept_filter(ipc::Request&, ipc::Response&) {
        const int st = coop_ldn_state(n_);
        return st == COOP_LS_AP || st == COOP_LS_AP_CREATED ? 0 : kInvalidState;
    }
    uint32_t open_station(ipc::Request&, ipc::Response&) { return coop_ldn_open_station(n_); }
    uint32_t close_station(ipc::Request&, ipc::Response&) { return left(coop_ldn_close_station(n_)); }
    uint32_t connect(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        buf_read(req.in_buf(0), &ni, sizeof ni);
        const SecurityConfig sec = arg_at<SecurityConfig>(req, 0);
        const UserConfig user = arg_at<UserConfig>(req, kConnectUser);
        forget();
        return coop_ldn_connect(n_, &ni, &sec, &user, req.arg<int32_t>(kConnectVersion), req.arg<int32_t>(kConnectVersion + 4));
    }
    uint32_t connect_private(ipc::Request&, ipc::Response&) {
        NXLOG_W("net", "ldn: ConnectPrivate has no co-op counterpart: ConnectNotFound");
        return kConnectNotFound;
    }
    uint32_t disconnect(ipc::Request&, ipc::Response&) { return left(coop_ldn_disconnect(n_)); }
    uint32_t initialize(ipc::Request&, ipc::Response&) { return coop_ldn_initialize(n_); }
    uint32_t finalize(ipc::Request&, ipc::Response&) {
        if (coop_ldn_state(n_) == COOP_LS_NONE) return kInvalidState;
        shutdown();
        return 0;
    }

    coop_node* n_;
    std::mutex mu_;
    std::array<uint32_t, 8> seen_{};    // node addresses of the last NetworkInfo the game read (0 = empty slot)
    std::array<uint8_t, 8> latest_{};   // NodeLatestUpdate flags since the last GetNetworkInfoLatestUpdate
};

}  // namespace nxrt::services::ldn_impl
