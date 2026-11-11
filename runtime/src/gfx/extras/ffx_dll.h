// SPDX-License-Identifier: GPL-3.0-or-later
// Where the AMD FidelityFX API DLLs are, for the two users of the API: temporal_ffx.cpp (FSR 3.1 upscaling) and framegen_ffx.cpp (FSR 3.1 frame
// generation). The v2.4.0 loader routes each effect to its own DLL in its own folder.
#pragma once
#include <string>

namespace nxrt::extras {

// The loader DLL, in order: [upscale] fsr_dll, then the release package's upscalers\fsr3\ folder next to the executable, then the executable's
// own folder; in each the v2.4.0 loader (amd_fidelityfx_loader_vk.dll) and then the v1.1.4 single DLL. Empty: none found.
std::wstring ffx_find_dll();

// True when `effect_dll` (for example amd_fidelityfx_framegeneration_vk.dll) is in the folder of the loader at `loader`.
bool ffx_effect_dll_exists(const std::wstring& loader, const wchar_t* effect_dll);

}  // namespace nxrt::extras
