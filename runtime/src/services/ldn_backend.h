// SPDX-License-Identifier: GPL-3.0-or-later
// The ldn:u backends behind one interface. ldn.cpp's LdnService picks one per service object ([net] protocol) and forwards to it; each
// backend owns its process-wide state and puts it back with reset() (ldn_reset). Backends: linkplay (Ldn on the relay lobby,
// ldn_linkplay.cpp) and native (NativeLdn on the co-op module, ldn_native.cpp; with the module off it answers with an offline Ldn).
#pragma once
#include <memory>

#include "nxrt/services.h"

namespace nxrt::services::ldn_impl {

// One IUserLocalCommunicationService object's command handlers. shutdown(): the object was released (leave the network, drop links).
class LdnSession {
public:
    virtual ~LdnSession() = default;
    virtual void shutdown() = 0;
};

class LdnBackend {
public:
    virtual ~LdnBackend() = default;
    virtual const char* name() const = 0;
    // A new service object: adds the nn::ldn command table to s and returns the session those commands run on (LdnService keeps it and
    // shuts it down on close).
    virtual std::shared_ptr<LdnSession> open(ipc::Service& s) = 0;
    // Back to the state before the first service object: lobbies, relay clients, discovery cache, co-op node (whatever the backend made).
    // network_reset calls it on every backend, also the one not in use.
    virtual void reset() = 0;
};

LdnBackend& linkplay_backend();   // ldn_linkplay.cpp
LdnBackend& native_backend();     // ldn_native.cpp

}  // namespace nxrt::services::ldn_impl
