// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 left out of the build (NXRT_UPSCALER_FSR3=OFF, cmake/gfx.cmake): the temporal.h backend reports itself unavailable, so
// temporal_evaluate records nothing in that mode.
#include "extras/temporal.h"

namespace nxrt::extras {

bool temporal_ffx_available() { return false; }
TemporalBackend* temporal_ffx(void*) { return nullptr; }

}  // namespace nxrt::extras
