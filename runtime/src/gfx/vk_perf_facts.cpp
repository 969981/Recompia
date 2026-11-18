// SPDX-License-Identifier: GPL-3.0-or-later
// The chosen GPU's facts for the performance report: see vk_perf_facts.h.
#include "vk_perf_facts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/host_device.h"
#include "nxrt/perf_report.h"

namespace nxrt::gfx {
namespace {

// VkPhysicalDeviceProperties::driverVersion is packed the vendor's way: NVIDIA 10.8.8.6 bits (581.57 = major 581, minor 57), Intel on
// Windows 18.14 bits (the last two parts of 31.0.101.5186), everyone else (AMD, Mesa) as VK_MAKE_API_VERSION.
std::string driver_version(uint32_t vendor, uint32_t v) {
    char b[48];
    if (vendor == 0x10de)
        std::snprintf(b, sizeof b, "%u.%02u", (v >> 22) & 0x3ff, (v >> 14) & 0xff);
    else if (vendor == 0x8086 && !host_is_linux())
        std::snprintf(b, sizeof b, "%u.%u", v >> 14, v & 0x3fff);
    else
        std::snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return b;
}

const char* type_text(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "other";
    }
}

bool has_extension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ex(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ex.data());
    for (const auto& e : ex) if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

}  // namespace

void vk_publish_perf_facts(VkPhysicalDevice pd) {
    if (!perf::enabled() || !pd) return;
    // A physical-device property struct of a device extension may be chained when the device supports the extension; it need not be enabled.
    const bool pci = has_extension(pd, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    VkPhysicalDevicePCIBusInfoPropertiesEXT pi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, pci ? &pi : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const VkPhysicalDeviceProperties& p = p2.properties;

    perf::GpuFacts f;
    f.name = p.deviceName;
    f.vendor_id = p.vendorID;
    f.device_id = p.deviceID;
    f.discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    f.type = type_text(p.deviceType);
    f.driver = driver_version(p.vendorID, p.driverVersion);
    if (dp.driverName[0] || dp.driverInfo[0]) {
        f.driver += " (";
        f.driver += dp.driverName;
        if (dp.driverInfo[0]) f.driver += std::string(dp.driverName[0] ? ", " : "") + dp.driverInfo;
        f.driver += ")";
    }
    char api[32];
    std::snprintf(api, sizeof api, "%u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    f.api = api;
    if (pci) {
        f.pci_valid = true;
        f.pci_domain = pi.pciDomain;
        f.pci_bus = pi.pciBus;
        f.pci_device = pi.pciDevice;
        f.pci_function = pi.pciFunction;
    }
    // Video memory: the largest device-local heap. Resizable BAR: the largest device-local heap that a host-visible memory type lives in
    // (without it, a ~256 MiB window: 214-256 MiB depending on the driver; with it, all of the video memory).
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t h = 0; h < mp.memoryHeapCount; ++h) {
        if (!(mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        f.vram_bytes = std::max<uint64_t>(f.vram_bytes, mp.memoryHeaps[h].size);
        for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
            const VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
            if (mp.memoryTypes[t].heapIndex == h && (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                f.bar_bytes = std::max<uint64_t>(f.bar_bytes, mp.memoryHeaps[h].size);
        }
    }
    perf::set_gpu(f);
}

}  // namespace nxrt::gfx
