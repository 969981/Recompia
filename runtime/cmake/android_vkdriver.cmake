# Android only: libadrenotools (BSD-2-Clause, github.com/bylaws/libadrenotools) for player-installed Vulkan drivers
# ([gfx] vulkan_driver_dir, src/os/os_android_vkdriver.cpp). Nothing links it: the runtime dlopens libadrenotools.so only when
# that key is set, because its linkernsbypass part (BSD-2-Clause, github.com/bylaws/liblinkernsbypass) runs a constructor at
# load that walks the linker's code, and players on the system driver must never run it.
# Output, next to libpkp_rt.so for the APK's jniLibs: libadrenotools.so plus the hook libraries it loads from nativeLibraryDir
# (libhook_impl.so, libmain_hook.so, libfile_redirect_hook.so, libgsl_alloc_hook.so). The app must extract native libraries
# (useLegacyPackaging = true), which the contract already requires for the on-device tools.
include_guard(GLOBAL)
if(NOT ANDROID)
    return()
endif()

include(FetchContent)
# bylaws/libadrenotools master 2024-09-10 "Update linkernsbypass" (16 KiB page fix); submodule lib/linkernsbypass aa39758
FetchContent_Declare(adrenotools
    GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
    GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
    GIT_SUBMODULES_RECURSE ON
    EXCLUDE_FROM_ALL)
set(_nxrt_bsl "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS ON)   # libadrenotools.so; its own CMakeLists keeps linkernsbypass static and hides it (--exclude-libs)
FetchContent_MakeAvailable(adrenotools)
set(BUILD_SHARED_LIBS "${_nxrt_bsl}")

set(_nxrt_vkdriver_libs adrenotools hook_impl main_hook file_redirect_hook gsl_alloc_hook)
set_target_properties(${_nxrt_vkdriver_libs} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_custom_target(nxrt_vkdriver_libs DEPENDS ${_nxrt_vkdriver_libs})
add_dependencies(nxrt nxrt_vkdriver_libs)   # built with the runtime; still not linked into it
