// SPDX-License-Identifier: GPL-3.0-or-later
// The ldn HLE's process lobby (services/ldn.cpp). By default it is built on first use from the config: [net] server (auto = the default:
// host on a relay in this process, find hosts on the LAN; empty or off = local wireless off; host:port = that relay), [net] password,
// [account] nickname.
#pragma once
#include <memory>

#include "../net/lobby.h"

struct coop_node;   // coop.h

namespace nxrt::services {

// [net] protocol = native: the process's co-op node (third_party/pokopia-coop) once an ldn:u service object made it, else null. The bsd
// service routes the game's datagram sockets to it (coop_vnet_*).
coop_node* ldn_native_node();

}  // namespace nxrt::services
