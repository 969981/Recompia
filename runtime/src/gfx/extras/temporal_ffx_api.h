// SPDX-License-Identifier: GPL-3.0-or-later
// The FidelityFX API entry points temporal_ffx.cpp calls, and where they come from: the signed Vulkan DLLs on Windows
// (temporal_ffx_dll.cpp), the SDK's sources built into the runtime on Linux (temporal_ffx_linked.cpp, cmake/ffx_sdk.cmake). The
// headers are the SDK v2.4.0 ones (third_party/amd_ffx_api); the Linux build's v1.1.4 sources share their C ABI.
#pragma once

#if !defined(_WIN32)
#define __declspec(x)   // ffx_api.h marks its entry points __declspec(dllexport), an MSVC spelling with no meaning here
#endif
#include "../../../third_party/amd_ffx_api/api/include/ffx_api.h"
#include "../../../third_party/amd_ffx_api/api/include/ffx_api_types.h"
#include "../../../third_party/amd_ffx_api/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/amd_ffx_api/upscalers/include/ffx_upscale.h"
#if !defined(_WIN32)
#undef __declspec
#endif

namespace nxrt::extras {

struct FfxApi {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

bool ffx_api_present();           // the API can be had, without loading it (the settings page's "available")
bool ffx_api_load(FfxApi* api);   // fills every entry point and logs where they came from, or logs why not and returns false

}  // namespace nxrt::extras
