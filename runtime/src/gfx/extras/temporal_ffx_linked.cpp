// SPDX-License-Identifier: GPL-3.0-or-later
// Linux: the FidelityFX API is built into the runtime from the SDK's sources (FSR 3 upscaler and its Vulkan backend only,
// cmake/ffx_sdk.cmake), so its entry points are plain calls and it is always there.
#include "extras/temporal_ffx_api.h"
#include "nxrt/log.h"

namespace nxrt::extras {

bool ffx_api_present() { return true; }

bool ffx_api_load(FfxApi* api) {
    api->create = &ffxCreateContext;
    api->destroy = &ffxDestroyContext;
    api->query = &ffxQuery;
    api->dispatch = &ffxDispatch;
    NXLOG_I("fsr3", "FidelityFX API built in (FidelityFX SDK v1.1.4 sources, Vulkan backend)");
    return true;
}

}  // namespace nxrt::extras
