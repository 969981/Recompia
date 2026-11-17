// SPDX-License-Identifier: GPL-3.0-or-later
// Performance report: the system block (perf_system.cpp). Logs, once, what the machine is: CPU (cpuid brand, cores, threads), memory, OS
// (Wine/Proton with the Linux kernel when the host kernel is Linux), laptop / desktop / handheld, mains or battery, the power plan, the
// GPU with its driver, PCIe link (current and maximum generation and width), resizable BAR and video memory. Nothing that identifies the
// player: no computer or user name, serial number, MAC address or path.
#pragma once
#include <string>

#include "nxrt/perf_report.h"

namespace nxrt::diag {

void perf_log_system(const perf::GpuFacts& gpu);
// The GPU's PCIe link now: "gen 4 x16 (max gen 4 x16)", or "unknown" (no PCI address, no readable property: Wine's device tree).
std::string perf_pcie_link(const perf::GpuFacts& gpu);

}  // namespace nxrt::diag
