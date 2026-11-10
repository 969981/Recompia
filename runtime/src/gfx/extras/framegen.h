// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 frame generation through the FidelityFX API (framegen_ffx.cpp; framegen_ffx_off.cpp when the build leaves FSR 3 out, cmake/gfx.cmake).
//
// It generates a frame between two of the game's: the 3D picture only, the HUD is taken from the real frames. The pieces it needs are not the
// engine's own: a swapchain made by the FidelityFX API (it replaces the engine's, and presents the generated frames), four queues (the device
// makes them, gfx.h Device::framegen_queues) and the FidelityFX frame-generation DLL, which the game does not ship: the player puts it next to the
// loader the upscaler uses.
#pragma once

namespace nxrt::extras {

// True when amd_fidelityfx_framegeneration_vk.dll is in the folder of the FidelityFX loader (ffx_dll.h). Needed before the device is made: the
// queues are reserved with it.
bool framegen_dll_found();

// What the F1 page and the launcher tell a player who tries to switch frame generation on without the DLL.
const char* framegen_missing_text();

}  // namespace nxrt::extras
