// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 frame generation through the FidelityFX API (framegen.h). The loader and the effect DLLs are the ones the upscaler uses
// (ffx_dll.h): the v2.4.0 loader loads amd_fidelityfx_framegeneration_vk.dll from its own folder.
#include "extras/framegen.h"

#include "extras/ffx_dll.h"

namespace nxrt::extras {

bool framegen_dll_found() {
    const std::wstring loader = ffx_find_dll();
    return !loader.empty() && ffx_effect_dll_exists(loader, L"amd_fidelityfx_framegeneration_vk.dll");
}

const char* framegen_missing_text() {
    return "Frame generation needs amd_fidelityfx_framegeneration_vk.dll (AMD FSR SDK v2.4.0, signedbin) in the game's upscalers\\fsr3 folder, "
           "next to amd_fidelityfx_loader_vk.dll. Put it there and restart the game.";
}

}  // namespace nxrt::extras
