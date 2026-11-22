// SPDX-License-Identifier: GPL-3.0-or-later
// Performance report, system block: see perf_system.h. Every probe answers "unknown" when it cannot read its source (Wine has no PCI
// properties in its device tree, a POSIX host no power plan); none of them can fail the start.
#include "perf_system.h"

#include <windows.h>
#if defined(_WIN32)
#include <cfgmgr32.h>
#include <devpropdef.h>
#pragma comment(lib, "cfgmgr32.lib")
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif
#include <intrin.h>   // __cpuid (MSVC; compat/intrin.h on the POSIX hosts)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/host_device.h"
#include "nxrt/log.h"

namespace nxrt::diag {
namespace {

constexpr const char* kTag = "perf";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

// A file of the Linux kernel or system: read where it is on the native build, through Wine's Z: drive (the host's root) on Windows.
std::string host_file(const std::string& linux_path) {
#if defined(_WIN32)
    std::string p = "Z:" + linux_path;
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return linux_path;
#endif
}

std::string first_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    return trim(s);
}

// PRETTY_NAME of os-release ("SteamOS", "Ubuntu 24.04.1 LTS").
std::string os_release_name() {
    std::ifstream f(host_file("/etc/os-release"));
    std::string line;
    while (f && std::getline(f, line)) {
        line = trim(line);
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string v = line.substr(12);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

std::string cpu_brand() {
    int r[4] = {};
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) < 0x80000004u) return "unknown";
    char b[49] = {};
    for (unsigned i = 0; i < 3; i++) {
        __cpuid(r, static_cast<int>(0x80000002u + i));
        std::memcpy(b + 16 * i, r, 16);
    }
    std::string s = trim(b);
    return s.empty() ? "unknown" : s;
}

std::string cpu_cores() {
    char b[160];
#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    uint32_t cores = 0, threads = 0;
    std::map<int, uint32_t> by_class;       // EfficiencyClass -> cores (hybrid CPUs: performance and efficiency cores)
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
        for (DWORD off = 0; off < len;) {
            const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            cores++;
            by_class[e->Processor.EfficiencyClass]++;
            for (WORD g = 0; g < e->Processor.GroupCount; g++) threads += static_cast<uint32_t>(__popcnt64(e->Processor.GroupMask[g].Mask));
            off += e->Size;
        }
    }
    if (!cores) return "cores unknown";
    std::snprintf(b, sizeof b, "%u cores / %u threads", cores, threads);
    std::string s = b;
    if (by_class.size() == 2) {
        std::snprintf(b, sizeof b, " (%u performance + %u efficiency cores)", by_class.rbegin()->second, by_class.begin()->second);
        s += b;
    }
    return s;
#else
    std::snprintf(b, sizeof b, "%u threads", std::thread::hardware_concurrency());
    return b;
#endif
}

std::string memory_total() {
    double gib = 0;
#if defined(_WIN32)
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) gib = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) gib = static_cast<double>(pages) * static_cast<double>(page) / (1024.0 * 1024.0 * 1024.0);
#endif
    if (!(gib > 0)) return "memory unknown";
    char b[64];
    std::snprintf(b, sizeof b, "memory %.1f GiB", gib);
    return b;
}

std::string linux_os() {
    std::string s = "Linux " + first_line(host_file("/proc/sys/kernel/osrelease"));
    const std::string distro = os_release_name();
    if (!distro.empty()) s += " (" + distro + ")";
    return s;
}

std::string os_name() {
#if defined(_WIN32)
    if (host_is_linux()) {
        std::string s = "Wine/Proton on " + linux_os();
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            using wine_version_fn = const char* (*)();
            if (const auto fn = reinterpret_cast<wine_version_fn>(GetProcAddress(nt, "wine_get_version")))
                if (const char* v = fn()) s += std::string(", Wine ") + v;
        }
        return s;
    }
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof vi;
    using rtl_get_version_fn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    const auto fn = nt ? reinterpret_cast<rtl_get_version_fn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (!fn || fn(&vi) != 0) return "Windows (version unknown)";
    char display[32] = {};
    {
        wchar_t w[32] = {};
        DWORD size = sizeof w;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", RRF_RT_REG_SZ, nullptr, w, &size) == ERROR_SUCCESS)
            WideCharToMultiByte(CP_UTF8, 0, w, -1, display, sizeof display, nullptr, nullptr);
    }
    DWORD ubr = 0, size = sizeof ubr;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    char b[128];
    std::snprintf(b, sizeof b, "Windows %s%s%s (build %lu.%lu)", vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000 ? "11" : std::to_string(vi.dwMajorVersion).c_str(),
                  *display ? " " : "", display, static_cast<unsigned long>(vi.dwBuildNumber), static_cast<unsigned long>(ubr));
    return b;
#else
    if (host_is_linux()) return linux_os();
    utsname u = {};
    if (uname(&u) != 0) return "unknown";
    return std::string(u.sysname) + " " + u.release;
#endif
}

// Laptop or desktop (a system battery is there), on mains or battery, the Windows power plan and power mode.
std::string power() {
    std::string s;
    if (host_is_steam_deck()) s = "machine handheld (Steam Deck)";
    else if (host_is_handheld()) s = "machine handheld";
#if defined(_WIN32)
    SYSTEM_POWER_STATUS ps = {};
    const bool ok = GetSystemPowerStatus(&ps) != 0;
    const bool battery = ok && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
    if (s.empty()) s = !ok || ps.BatteryFlag == 255 ? "machine unknown" : battery ? "machine laptop (has a battery)" : "machine desktop (no battery)";
    if (!ok || ps.ACLineStatus == 255) s += ", power source unknown";
    else if (ps.ACLineStatus == 1) s += ", on mains";
    else {
        char b[48];
        if (ps.BatteryLifePercent <= 100) std::snprintf(b, sizeof b, ", ON BATTERY (%u%%)", static_cast<unsigned>(ps.BatteryLifePercent));
        else std::snprintf(b, sizeof b, ", ON BATTERY");
        s += b;
    }
    std::string plan = "unknown", mode;
    if (HMODULE pp = LoadLibraryW(L"powrprof.dll")) {
        using active_fn = DWORD(WINAPI*)(HKEY, GUID**);
        using name_fn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
        using overlay_fn = DWORD(WINAPI*)(GUID*);
        const auto active = reinterpret_cast<active_fn>(GetProcAddress(pp, "PowerGetActiveScheme"));
        const auto name = reinterpret_cast<name_fn>(GetProcAddress(pp, "PowerReadFriendlyName"));
        const auto overlay = reinterpret_cast<overlay_fn>(GetProcAddress(pp, "PowerGetActualOverlayScheme"));
        GUID* scheme = nullptr;
        if (active && name && active(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
            wchar_t w[128] = {};
            DWORD size = sizeof w;
            if (name(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR*>(w), &size) == ERROR_SUCCESS) {
                char u[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, nullptr, nullptr);
                if (*u) plan = u;
            }
            LocalFree(scheme);
        }
        GUID ov = {};
        if (overlay && overlay(&ov) == ERROR_SUCCESS) {
            // Windows 10/11 power mode (the slider over the plan)
            static const struct { GUID g; const char* name; } kModes[] = {
                {{0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}}, "best power efficiency"},
                {{0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}}, "better battery"},
                {{0x00000000, 0x0000, 0x0000, {0, 0, 0, 0, 0, 0, 0, 0}}, "balanced"},
                {{0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}}, "best performance"},
            };
            mode = "other";
            for (const auto& m : kModes) if (std::memcmp(&m.g, &ov, sizeof ov) == 0) mode = m.name;
        }
        FreeLibrary(pp);
    }
    s += ", power plan " + plan;
    if (!mode.empty()) s += ", power mode " + mode;
#else
    if (s.empty()) s = "machine unknown";
#endif
    return s;
}

// PCIe generation of a link speed in GT/s (2.5, 5, 8, 16, 32, 64).
int gen_of_gts(double gts) {
    static const double kGts[] = {2.5, 5.0, 8.0, 16.0, 32.0, 64.0};
    for (int i = 0; i < 6; i++) if (gts > kGts[i] * 0.9 && gts < kGts[i] * 1.1) return i + 1;
    return 0;
}

struct Link {
    int cur_gen = 0, cur_width = 0, max_gen = 0, max_width = 0;
};

bool link_sysfs(const perf::GpuFacts& g, Link* l) {
    char dir[64];
    std::snprintf(dir, sizeof dir, "/sys/bus/pci/devices/%04x:%02x:%02x.%x/", g.pci_domain, g.pci_bus, g.pci_device, g.pci_function);
    const std::string base = dir;
    const std::string cs = first_line(host_file(base + "current_link_speed")), ms = first_line(host_file(base + "max_link_speed"));
    const std::string cw = first_line(host_file(base + "current_link_width")), mw = first_line(host_file(base + "max_link_width"));
    if (cs.empty() || cw.empty()) return false;
    l->cur_gen = gen_of_gts(std::atof(cs.c_str()));       // "16.0 GT/s PCIe"
    l->max_gen = gen_of_gts(std::atof(ms.c_str()));
    l->cur_width = std::atoi(cw.c_str());
    l->max_width = std::atoi(mw.c_str());
    return true;
}

#if defined(_WIN32)
// The device tree's PCI properties of the display adapter at the GPU's bus / device / function (DEVPKEY_Device_BusNumber, _Address) and
// its link (DEVPKEY_PciDevice_CurrentLinkSpeed / Width, MaxLinkSpeed / Width: speed 1..6 = generation, width = lanes). Defined here, as in
// devpkey.h / pciprop.h, so no INITGUID translation unit is needed.
constexpr DEVPROPKEY kBusNumber = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 23};
constexpr DEVPROPKEY kAddress = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 30};
constexpr GUID kPciProps = {0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}};
constexpr DEVPROPKEY kCurSpeed = {kPciProps, 9}, kCurWidth = {kPciProps, 10}, kMaxSpeed = {kPciProps, 11}, kMaxWidth = {kPciProps, 12};

bool dev_u32(DEVINST dn, const DEVPROPKEY& key, uint32_t* out) {
    DEVPROPTYPE type = 0;
    ULONG size = sizeof *out;
    return CM_Get_DevNode_PropertyW(dn, &key, &type, reinterpret_cast<PBYTE>(out), &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_UINT32;
}

bool link_devtree(const perf::GpuFacts& g, Link* l) {
    const wchar_t* const display_class = L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    const ULONG flags = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, display_class, flags) != CR_SUCCESS || len == 0) return false;
    std::vector<wchar_t> ids(len);
    if (CM_Get_Device_ID_ListW(display_class, ids.data(), len, flags) != CR_SUCCESS) return false;
    for (const wchar_t* id = ids.data(); *id; id += wcslen(id) + 1) {
        DEVINST dn = 0;
        if (CM_Locate_DevNodeW(&dn, const_cast<wchar_t*>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        uint32_t bus = 0, addr = 0;
        if (!dev_u32(dn, kBusNumber, &bus) || !dev_u32(dn, kAddress, &addr)) continue;
        if (bus != g.pci_bus || addr != (g.pci_device << 16 | g.pci_function)) continue;
        uint32_t cs = 0, cw = 0, ms = 0, mw = 0;
        if (!dev_u32(dn, kCurSpeed, &cs) || !dev_u32(dn, kCurWidth, &cw)) return false;
        dev_u32(dn, kMaxSpeed, &ms);
        dev_u32(dn, kMaxWidth, &mw);
        l->cur_gen = static_cast<int>(cs);
        l->cur_width = static_cast<int>(cw);
        l->max_gen = static_cast<int>(ms);
        l->max_width = static_cast<int>(mw);
        return true;
    }
    return false;
}
#endif

std::string mib(uint64_t bytes) { return std::to_string(bytes >> 20) + " MiB"; }

}  // namespace

bool perf_gpu_load(GpuLoad* out) {
    static const std::string dir = host_file("/sys/class/kgsl/kgsl-3d0/");
    const std::string busy = first_line(dir + "gpu_busy_percentage");     // "82 %"
    if (busy.empty() || !std::isdigit(static_cast<unsigned char>(busy[0]))) return false;
    out->busy = std::clamp(std::atof(busy.c_str()) / 100.0, 0.0, 1.0);
    out->mhz = std::atoi(first_line(dir + "clock_mhz").c_str());
    out->max_mhz = std::atoi(first_line(dir + "max_clock_mhz").c_str());
    return true;
}

std::string perf_pcie_link(const perf::GpuFacts& g) {
    if (!g.pci_valid) return "unknown (the driver gives no PCI address)";
    Link l;
    bool ok = false;
#if defined(_WIN32)
    ok = host_is_linux() ? link_sysfs(g, &l) : link_devtree(g, &l);
#else
    ok = host_is_linux() && link_sysfs(g, &l);
#endif
    if (!ok || !l.cur_gen) return "unknown";
    char b[96];
    std::snprintf(b, sizeof b, "gen %d x%d (max gen %d x%d)", l.cur_gen, l.cur_width, l.max_gen, l.max_width);
    std::string s = b;
    if (l.max_gen && l.max_width && (l.cur_width < l.max_width)) s += ", FEWER LANES THAN THE CARD HAS";
    return s;
}

void perf_log_system(const perf::GpuFacts& g) {
    NXLOG_I(kTag, "system: CPU %s, %s; %s; OS %s", cpu_brand().c_str(), cpu_cores().c_str(), memory_total().c_str(), os_name().c_str());
    NXLOG_I(kTag, "system: %s", power().c_str());
    std::string bar;
    if (!g.discrete) bar = "n/a (not a discrete GPU)";
    else if (!g.bar_bytes) bar = "no host-visible video memory";
    else if (g.bar_bytes <= perf::kBarWindowMaxBytes) bar = "off (" + mib(g.bar_bytes) + " host-visible video memory)";
    else bar = "on (" + mib(g.bar_bytes) + " host-visible video memory)";
    char ids[32];
    std::snprintf(ids, sizeof ids, "%04x:%04x", g.vendor_id, g.device_id);
    NXLOG_I(kTag, "system: GPU %s (%s, %s), driver %s, Vulkan %s; video memory %.1f GiB; resizable BAR %s; PCIe link at start %s (an idle GPU may run its link slower)",
            g.name.c_str(), g.type.c_str(), ids, g.driver.c_str(), g.api.c_str(), static_cast<double>(g.vram_bytes) / (1024.0 * 1024.0 * 1024.0), bar.c_str(),
            perf_pcie_link(g).c_str());
}

}  // namespace nxrt::diag
