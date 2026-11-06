// SPDX-License-Identifier: GPL-3.0-or-later
// The ldn HLE's process lobby (services/ldn.cpp). By default it is built on first use from the config: [net] server (auto = the default:
// host on a relay in this process, find hosts on the LAN; empty or off = local wireless off; host:port = that relay), [net] password,
// [account] nickname. Tests put their own lobby in place.
#pragma once
#include <memory>

#include "../net/lobby.h"

namespace nxrt::services {

// Replaces the lobby the ldn service uses from its next Initialize on (null = local wireless off). network_reset() drops it and goes back
// to the config.
void ldn_set_lobby(std::shared_ptr<vnet::Lobby> lobby);

}  // namespace nxrt::services
