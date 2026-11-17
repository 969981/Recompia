// SPDX-License-Identifier: GPL-3.0-or-later
// The chosen GPU's facts for the performance report (vk_perf_facts.cpp, nxrt/perf_report.h): name, type, driver version as the vendor
// writes it, Vulkan version, PCI address (VK_EXT_pci_bus_info), video memory and the host-visible part of it (resizable BAR).
#pragma once
#include <volk.h>

namespace nxrt::gfx {

// Reads the facts of `pd` and hands them to perf::set_gpu (which logs the system block once). A no-op with the report off.
void vk_publish_perf_facts(VkPhysicalDevice pd);

}  // namespace nxrt::gfx
