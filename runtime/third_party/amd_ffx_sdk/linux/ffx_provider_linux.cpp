// SPDX-License-Identifier: MIT
// Recompia: the provider list of the FidelityFX API for the Linux build, in place of the SDK's ffx-api/src/ffx_provider.cpp
// (that file also asks the AMD Windows driver, amdxc64.dll through D3D12, for newer providers). Only the FSR 3 upscaler is built
// here, so it is the one provider; the three functions keep ffx_provider.cpp's behaviour for a list without driver providers.
// The Vulkan backend's interface also names the frame generation swap chain's configure call (FrameInterpolationSwapchainVK.cpp, not
// built): without frame generation it has nothing to configure.
#include "ffx_provider.h"
#include "ffx_provider_fsr3upscale.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>

namespace {
const ffxProvider* const kProviders[] = {&ffxProvider_FSR3Upscale::Instance};
}

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void*)
{
    for (const ffxProvider* p : kProviders)
        if ((!overrideId || p->GetId() == overrideId) && p->CanProvide(descType)) return p;
    return nullptr;
}

const ffxProvider* GetAssociatedProvider(ffxContext* context)
{
    return reinterpret_cast<const InternalContextHeader*>(*context)->provider;
}

uint64_t GetProviderVersions(ffxStructType_t descType, void*, uint64_t capacity, uint64_t* versionIds, const char** versionNames)
{
    uint64_t count = 0;
    for (const ffxProvider* p : kProviders) {
        if (count >= capacity) break;
        if (!p->CanProvide(descType)) continue;
        if (versionIds) versionIds[count] = p->GetId();
        if (versionNames) versionNames[count] = p->GetVersionName();
        ++count;
    }
    return count;
}

uint64_t GetProviderCount(ffxStructType_t descType, void* device)
{
    return GetProviderVersions(descType, device, UINT64_MAX, nullptr, nullptr);
}

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*)
{
    return FFX_ERROR_INVALID_ARGUMENT;
}
