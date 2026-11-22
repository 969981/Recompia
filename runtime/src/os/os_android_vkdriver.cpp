// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_vkdriver.cpp: a player-installed Vulkan driver on Android (os_android_vkdriver.h). Android only (the guard
// below); libadrenotools and its hook libraries are built by cmake/android_vkdriver.cmake and only dlopened from here.
//
// How libadrenotools loads a driver: it copies /system/lib64/libvulkan.so into a memfd under a new soname, opens that copy in
// a linker namespace whose search path is the app's nativeLibraryDir (where its hook libraries are), and the hook answers the
// copy's android_load_sphal_library("vulkan.*.so") with the custom library from customDriverDir instead of the vendor driver.
// The handle it returns is that private loader; its vkGetInstanceProcAddr drives everything after (volkInitializeCustom).
#if defined(__ANDROID__)   // the runtime's source glob also hands this file to desktop builds: compile it to nothing there
#define VK_USE_PLATFORM_ANDROID_KHR
#include "os_android_vkdriver.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include <fstream>
#include <sstream>
#include <string>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::os {
namespace {

constexpr const char* kTag = "gfx";
constexpr int kAdrenotoolsDriverCustom = 1 << 0;   // ADRENOTOOLS_DRIVER_CUSTOM (include/adrenotools/priv.h)
using OpenLibvulkan = void* (*)(int, int, const char*, const char*, const char*, const char*, const char*, void**);

// The value of "libraryName" in meta.json (the driver zip layout of the Switch emulators); a bare file name or "".
std::string library_name(const std::string& dir) {
    std::ifstream f(dir + "meta.json");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string j = ss.str();
    size_t k = j.find("\"libraryName\"");
    if (k == std::string::npos) return {};
    k = j.find(':', k);
    const size_t a = k == std::string::npos ? k : j.find('"', k);
    const size_t b = a == std::string::npos ? a : j.find('"', a + 1);
    if (b == std::string::npos) return {};
    std::string name = j.substr(a + 1, b - a - 1);
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return {};
    return name;
}

// nativeLibraryDir: the folder this library was loaded from (the app is packaged with extracted libraries, useLegacyPackaging).
std::string own_dir() {
    Dl_info di{};
    if (!dladdr(reinterpret_cast<void*>(&custom_vulkan_gipa), &di) || !di.dli_fname) return {};
    std::string p = di.dli_fname;
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

bool has_gpu(PFN_vkGetInstanceProcAddr gipa) {
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) return false;
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    uint32_t n = 0;
    if (enumerate) enumerate(inst, &n, nullptr);
    if (destroy) destroy(inst, nullptr);
    return n > 0;
}

void* load() {
    std::string dir = config_get_str("gfx", "vulkan_driver_dir", "");
    if (dir.empty()) return nullptr;
    if (dir.back() != '/') dir += '/';   // libadrenotools concatenates dir + name
    const std::string name = library_name(dir);
    struct stat st{};
    if (name.empty() || ::stat((dir + name).c_str(), &st) != 0) {
        NXLOG_W(kTag, "custom Vulkan driver: %smeta.json names no library that exists there: using the system driver", dir.c_str());
        return nullptr;
    }
    const std::string hooks = own_dir();
    void* at = ::dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    auto open_vk = at ? reinterpret_cast<OpenLibvulkan>(::dlsym(at, "adrenotools_open_libvulkan")) : nullptr;
    if (!open_vk) {
        NXLOG_W(kTag, "custom Vulkan driver: libadrenotools.so is not in the app (%s): using the system driver", ::dlerror());
        return nullptr;
    }
    void* loader = open_vk(RTLD_NOW, kAdrenotoolsDriverCustom, nullptr, hooks.c_str(), dir.c_str(), name.c_str(), nullptr, nullptr);
    void* gipa = loader ? ::dlsym(loader, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa) {
        NXLOG_W(kTag, "custom Vulkan driver: libadrenotools could not open %s%s (hooks in %s): using the system driver", dir.c_str(),
                name.c_str(), hooks.c_str());
        return nullptr;
    }
    // A driver that loads but finds no GPU (a Turnip on an Adreno it does not know, a package for another GPU family) would end
    // the game at device creation: probe one instance through it and fall back to the system driver when it lists no device.
    if (!has_gpu(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa))) {
        NXLOG_W(kTag, "custom Vulkan driver: %s%s lists no GPU on this device: using the system driver", dir.c_str(), name.c_str());
        return nullptr;
    }
    // One pipeline cache file per driver: a driver rejects another driver's cache data and then overwrites the file, so sharing
    // [nvn] pipeline_cache would cost the system driver its whole cache every time the player switches (measured on an Adreno 750 phone).
    // An explicit [nvn] pipeline_cache (ini or env) still wins.
    if (!config_has("nvn", "pipeline_cache")) config_set("nvn", "pipeline_cache", ("pipeline_cache." + name + ".bin").c_str());
    // The hook falls back to the vendor driver when the custom one fails to load: the device line (driver name) says which ran.
    NXLOG_I(kTag, "custom Vulkan driver: %s%s through libadrenotools (private loader copy, hooks in %s)", dir.c_str(), name.c_str(),
            hooks.c_str());
    return gipa;
}

}  // namespace

void* custom_vulkan_gipa() {
    static void* const gipa = load();
    return gipa;
}

bool custom_vulkan_create_surface(void* instance, void* native_window, uint64_t* surface_out) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(custom_vulkan_gipa());
    if (!gipa || !instance || !native_window || !surface_out) return false;
    auto inst = static_cast<VkInstance>(instance);
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(gipa(inst, "vkCreateAndroidSurfaceKHR"));
    if (!create) return false;
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = static_cast<ANativeWindow*>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    const VkResult r = create(inst, &ci, nullptr, &s);
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "custom Vulkan driver: vkCreateAndroidSurfaceKHR failed (%d)", static_cast<int>(r));
        return false;
    }
    *surface_out = reinterpret_cast<uint64_t>(s);
    return true;
}

void custom_vulkan_destroy_surface(void* instance, uint64_t surface) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(custom_vulkan_gipa());
    if (!gipa || !instance || !surface) return;
    auto inst = static_cast<VkInstance>(instance);
    if (auto destroy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(inst, "vkDestroySurfaceKHR")))
        destroy(inst, reinterpret_cast<VkSurfaceKHR>(surface), nullptr);
}

}  // namespace nxrt::os
#endif  // __ANDROID__
