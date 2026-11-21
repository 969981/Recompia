// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_vkdriver.h: a player-installed Vulkan driver on Android ([gfx] vulkan_driver_dir), loaded through libadrenotools.
//
// Off unless the player installed a driver: an empty key (the default) means the system driver, and nothing here is loaded.
// libadrenotools (BSD-2-Clause) is a separate libadrenotools.so in the APK, dlopened only when the key is set, because its
// linkernsbypass part runs a constructor at load that must not run for players on the system driver.
#pragma once
#include <cstdint>

namespace nxrt::os {

// vkGetInstanceProcAddr of a private copy of the system Vulkan loader that loads the driver in [gfx] vulkan_driver_dir (a folder
// in app-private storage with meta.json and the library its "libraryName" names), as a void*. nullptr when the key is empty or
// the driver does not load (logged): the caller then opens the system loader as before. Decided once per process.
void* custom_vulkan_gipa();

// The surface for an ANativeWindow through that private loader: SDL resolves vkCreateAndroidSurfaceKHR through the system
// loader, which cannot take an instance created by the private one. Valid only when custom_vulkan_gipa() is non-null.
bool custom_vulkan_create_surface(void* instance, void* native_window, uint64_t* surface_out);
void custom_vulkan_destroy_surface(void* instance, uint64_t surface);

}  // namespace nxrt::os
