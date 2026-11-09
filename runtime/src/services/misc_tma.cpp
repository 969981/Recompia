// SPDX-License-Identifier: GPL-3.0-or-later
// htc, htc:tenv and htcs: the TMA services, absent on retail (see misc.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

std::shared_ptr<Service> make_tma(const char* name, const CmdNames& cmds) {
    return make_iface(name, [cmds](Service& s) {
        for (const auto& c : cmds) s.add_cmd(c.first, c.second, refuse(kNotSupported, "tma", c.second));
    });
}
CmdNames htc_cmds() {
    return {{0, "GetEnvironmentVariable"},        {1, "GetEnvironmentVariableLength"}, {2, "BindHostConnectionEvent"},
            {3, "BindHostDisconnectionEvent"},    {4, "BindHostConnectionEventForSystem"}, {5, "BindHostDisconnectionEventForSystem"},
            {6, "GetBridgeIpAddress"},            {7, "GetBridgePort"},                {8, "SetCradleAttached"},
            {9, "GetBridgeSubnetMask"},           {10, "GetBridgeMacAddress"},         {11, "GetWorkingDirectoryPath"},
            {12, "GetWorkingDirectoryPathSize"},  {13, "RunOnHostStart"},              {14, "RunOnHostResults"},
            {15, "SetBridgeIpAddress"},           {16, "SetBridgeSubnetMask"},         {17, "SetBridgePort"},
            {18, "GetBridgeSerialNumber"},        {19, "GetBridgeFwVersion"},          {20, "ResetBridgeSettings"},
            {21, "BeginUpdateBridge"},            {22, "ContinueUpdateBridge"},        {23, "EndUpdateBridge"},
            {24, "GetBridgeType"},                {25, "GetBridgeDefaultGateway"},     {26, "SetBridgeDefaultGateway"},
            {27, "GetBridgeDhcp"},                {28, "SetBridgeDhcp"},               {29, "GetBridgeProgramVersion"}};
}
CmdNames htcs_cmds() {
    return {{0, "Socket"},  {1, "Close"},   {2, "Connect"},   {3, "Bind"},     {4, "Listen"},   {5, "Accept"},   {6, "Recv"},
            {7, "Send"},    {8, "Shutdown"}, {9, "Fcntl"},     {10, "GetPeerNameAny"}, {11, "GetDefaultHostName"}, {12, "CreateSocketOld"},
            {13, "CreateSocket"}, {100, "RegisterProcessId"}, {101, "MonitorManager"}, {130, "Cmd130"}, {131, "Cmd131"}};
}
CmdNames htc_tenv_cmds() { return {{0, "GetServiceInterface"}}; }

}  // namespace nxrt::services::miscsvc
