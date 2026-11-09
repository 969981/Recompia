// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u: picks the backend per service object ([net] protocol: native = ldn_native.cpp, linkplay = ldn_linkplay.cpp) and holds the
// service's entry points (the IUserServiceCreator, ldn_reset, ldnapi). Shared types: ldn_types.h; shared helpers: ldn_internal.h.
#include "ldn.h"

#include <memory>

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "ldn_linkplay.h"
#include "ldn_native.h"
#include "services_internal.h"

namespace nxrt::services {
using namespace ldn_impl;
namespace {

// [net] protocol picks the backend per service object: native = NativeLdn on the process's coop node (or, with the module off, Ldn with
// no lobby: the offline answers), linkplay = Ldn on the relay lobby.
class LdnService final : public ipc::Service {
public:
    LdnService() : ipc::Service("nn::ldn::detail::IUserLocalCommunicationService") {
        coop_node* n = native_protocol() ? native_node() : nullptr;
        if (n) {
            native_ = std::make_shared<NativeLdn>(n);
            native_->add_commands(*this);
        } else {
            ldn_ = std::make_shared<Ldn>(native_protocol());
            ldn_->add_commands(*this);
        }
    }
    void on_close() override {
        if (native_) native_->shutdown();
        if (ldn_) ldn_->shutdown();
    }

private:
    std::shared_ptr<Ldn> ldn_;
    std::shared_ptr<NativeLdn> native_;
};

}  // namespace

std::shared_ptr<ipc::Service> make_ldn_user_creator() {
    return make_iface("nn::ldn::detail::IUserServiceCreator", [](ipc::Service& s) {
        s.add_cmd(0, "CreateUserLocalCommunicationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            note(1000, "IUserServiceCreator CreateUserLocalCommunicationService");
            rsp.push_object(std::make_shared<LdnService>());
            return 0;
        });
        s.add_cmd(1, "CreateClientProcessMonitor", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            note(1001, "IUserServiceCreator CreateClientProcessMonitor");
            rsp.push_object(make_iface("nn::ldn::detail::IClientProcessMonitor", [](ipc::Service& m) {
                m.add_cmd(0, "RegisterClient", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
            }));
            return 0;
        });
    });
}

coop_node* ldn_native_node() { return native().node.load(); }

void ldn_reset() {
    {
        std::lock_guard<std::mutex> lk(g_seen_mu);
        g_seen.clear();
    }
    {
        Discovery& d = discovery();
        std::lock_guard<std::mutex> lk(d.mu);
        d.ran = false;
        d.found.clear();
        d.logged.clear();
    }
    std::shared_ptr<vnet::Lobby> old;
    {
        LobbySlot& s = slot();
        std::lock_guard<std::mutex> lk(s.mu);
        old.swap(s.lobby);
        s.built = false;
        s.auto_mode = false;
        s.target.clear();
        s.relays.clear();
    }
    ldnapi::set_session_variant(0, "");
    ldnapi::set_story_mod(false);
    {
        NativeSlot& n = native();
        coop_node* node = n.node.exchange(nullptr);
        {
            std::lock_guard<std::mutex> lk(n.mu);
            n.built = false;
            n.build.clear();
        }
        if (node) coop_destroy(node);   // finalizes: the host's links and the virtual sockets close
    }
    // old goes away here (a relay client closes its connection)
}

namespace ldnapi {
void set_session_variant(uint8_t v, const char* what) {
    v &= kVariantMask >> kVariantShift;
    Variant& s = variant();
    std::lock_guard<std::mutex> lk(s.mu);
    s.value = v;
    s.what = what ? what : "";
}
uint8_t session_variant() { return own_variant(); }
void set_story_mod(bool on) { story_flag().store(on); }
bool story_mod() { return story_flag().load(); }
bool native_protocol() { return services::native_protocol(); }
}  // namespace ldnapi

}  // namespace nxrt::services
