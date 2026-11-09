// SPDX-License-Identifier: GPL-3.0-or-later
// Offline network services, shared part: the process state, the first-use and "ask:" logs, and the command-table helpers (see network.cpp).
#include "network_internal.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"
#include "coop.h"
#include "ldn.h"
#include "services_internal.h"

namespace nxrt::services::netsvc {

NetState& st() {
    static NetState s;
    return s;
}

// Logs the first use of a command. One line per (service, command) for the life of the process (network_reset() clears it).
void note(const char* svc, uint32_t id, const char* name) {
    NetState& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.seen.insert(std::string(svc) + " " + std::to_string(id)).second) return;
    }
    NXLOG_I("net", "%s cmd %u %s: answered offline (first use)", svc, id, name);
}

// [net] online_test, read once: nifm and ntc report online (nothing reaches the network: bsd and BCAT answer as before).
bool online_test() {
    static const bool on = config_get_bool("net", "online_test", false);
    return on;
}

namespace {

constexpr int kMaxCallLogs = 50;

// Printable text of a 0x20-byte name field in a request's raw arguments ("?" when it is not plain text).
std::string raw_name(const ipc::Request& req, size_t off) {
    std::string out;
    for (size_t i = 0; i < 0x20 && off + i < req.in_size; ++i) {
        const uint8_t c = req.in[off + i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return "?";
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Logs one call of a command at Info, the first kMaxCallLogs calls of each (service, command id, name). BCAT directory and file opens and
// the sync requests with a directory name also log the name(s) they were given; no buffers, nothing else from the arguments.
void log_call(const char* svc, const std::string& iface, uint32_t id, const char* name, const ipc::Request& req) {
    // only the services events could depend on: bsd, ssl and the others stay off this path (bsd runs per packet in Link Play)
    if (std::strcmp(svc, "nifm") != 0 && std::strcmp(svc, "bcat") != 0 && std::strcmp(svc, "ntc") != 0) return;
    NetState& s = st();
    int n;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        n = ++s.calls[std::string(svc) + " " + iface + " " + std::to_string(id) + " " + name];
    }
    if (n > kMaxCallLogs) return;
    std::string arg;
    if (std::strcmp(svc, "bcat") == 0) {
        const std::string nm = name;
        if (nm == "Open") {
            arg = " path=\"" + raw_name(req, 0) + "\"";
            if (req.in_size >= 0x40) arg += " file=\"" + raw_name(req, 0x20) + "\"";
        } else if (nm.find("DirectoryName") != std::string::npos) {
            arg = " dir=\"" + raw_name(req, req.in_size >= 0x28 ? 8 : 0) + "\"";
        }
    }
    NXLOG_I("net", "ask: %s cmd %u %s (call %d)%s", svc, id, name, n, arg.c_str());
}

// "ask:" lines for what the game tries to reach: DNS names, connect targets, TLS host names. The first kMaxCallLogs of each kind; only
// the names and the address are logged, never a buffer or payload.
bool ask_budget(int kind) {
    static std::atomic<int> counts[3];
    return counts[kind].fetch_add(1) < kMaxCallLogs;
}

}  // namespace

// Printable ASCII text of an input buffer (up to 255 bytes, up to the first 0); "?" when it holds anything else.
std::string buf_text(const ipc::Buffer& b) {
    uint8_t tmp[255] = {};
    const uint64_t n = buf_read(b, tmp, b.size < sizeof tmp ? b.size : sizeof tmp);
    std::string out;
    for (uint64_t i = 0; i < n; ++i) {
        if (tmp[i] == 0) break;
        if (tmp[i] < 0x20 || tmp[i] > 0x7E) return "?";
        out.push_back(static_cast<char>(tmp[i]));
    }
    return out;
}

void ask_dns(const ipc::Request& req) {
    if (!ask_budget(0)) return;
    // switchbrew sfdnsres: GetAddrInfoRequest[WithOptions] takes the host name in input buffer 0 and the service name in buffer 1
    const std::string host = buf_text(req.in_buf(0));
    const std::string serv = buf_text(req.in_buf(1));
    NXLOG_I("net", "ask: dns host=\"%s\" service=\"%s\"", host.c_str(), serv.c_str());
}

void ask_connect(const vsock::Addr& a) {
    if (!ask_budget(1)) return;
    NXLOG_I("net", "ask: connect %u.%u.%u.%u:%u", (a.ip >> 24) & 255, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, static_cast<unsigned>(a.port));
}

void ask_ssl_host(const ipc::Request& req) {
    if (!ask_budget(2)) return;
    // switchbrew ISslConnection::SetHostName: the name in input buffer 0
    NXLOG_I("net", "ask: ssl host=\"%s\"", buf_text(req.in_buf(0)).c_str());
}

uint32_t alloc_client_id() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_client_id++;
}
uint64_t alloc_pki_id() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_pki_id++;
}
uint32_t alloc_cancel_handle() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_cancel_handle++;
}

namespace {

uint32_t run(const Row& r, ipc::Request& req, ipc::Response& rsp) {
    switch (r.k) {
        case K::Ok: return 0;
        case K::U8: rsp.push(static_cast<uint8_t>(r.v)); return 0;
        case K::U32: rsp.push(static_cast<uint32_t>(r.v)); return 0;
        case K::S32: rsp.push(static_cast<int32_t>(r.v)); return 0;
        case K::U64: rsp.push(static_cast<uint64_t>(r.v)); return 0;
        case K::Fail: return static_cast<uint32_t>(r.v);
        case K::Zeros: rsp.out.insert(rsp.out.end(), r.n, uint8_t{0}); return 0;
        case K::BufZero: {
            std::vector<uint8_t> z(r.n, 0);
            buf_write(req.out_buf(0), z.data(), r.n);
            return 0;
        }
        case K::Errno:
            rsp.push(static_cast<int32_t>(-1));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            for (uint32_t i = 0; i < r.n; ++i) rsp.push(static_cast<uint32_t>(0));
            return 0;
        case K::ErrnoOk:
            rsp.push(static_cast<int32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        case K::Bytes:
            for (uint32_t i = 0; i < r.n && i < 8; ++i) rsp.push(static_cast<uint8_t>(r.v >> (8 * i)));
            return 0;
    }
    return 0;
}

}  // namespace

// One command with its own handler; logs the first use.
void cmd(ipc::Service& s, const char* svc, uint32_t id, const char* name, ipc::Handler h) {
    s.add_cmd(id, name, [svc, iface = s.name(), id, name, h = std::move(h)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(svc, id, name);
        log_call(svc, iface, id, name, req);
        return h(req, rsp);
    });
}

// Every row of a table becomes a command.
void rows(ipc::Service& s, const char* svc, std::initializer_list<Row> list) {
    for (const Row& r : list) {
        Row row = r;
        cmd(s, svc, row.id, row.name, [row](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return run(row, req, rsp); });
    }
}

namespace {

// A service object whose release is observed (ssl context and connection counts). on_close runs when the last reference goes.
class Tracked final : public ipc::Service {
public:
    Tracked(const char* name, std::function<void()> released) : ipc::Service(name), released_(std::move(released)) {}
    void on_close() override {
        if (released_) released_();
    }

private:
    std::function<void()> released_;
};

}  // namespace

std::shared_ptr<ipc::Service> tracked(const char* name, std::function<void()> released, const std::function<void(ipc::Service&)>& setup) {
    auto s = std::make_shared<Tracked>(name, std::move(released));
    setup(*s);
    return s;
}

// Copy handles belong to the client (HIPC copies them: the guest closes its own afterwards), so there is nothing to close here (debug note 122).
void close_copied(const ipc::Request&) {}

}  // namespace nxrt::services::netsvc
