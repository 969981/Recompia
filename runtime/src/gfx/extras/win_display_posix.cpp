// SPDX-License-Identifier: GPL-3.0-or-later
// Display HDR query off Windows (win_display_posix.cpp), the counterpart of win_display.cpp. Same contract, same struct: the
// caller (src/gfx/vk_renderer.cpp) asks whether the monitor under the window has HDR on and what its luminance numbers
// are.
//
// Windows answers through the CCD database and DXGI_OUTPUT_DESC1. macOS has the same numbers through CoreGraphics:
// a display's EDR headroom is what the OS allows the layer to exceed SDR white by, and CGDisplayMode gives the pixel
// encoding. What is missing is the *user's* "SDR content brightness" slider, which Windows reports directly; on macOS
// that is not exposed, so sdr_white_nits stays at its default and the caller's tone mapping uses the headroom ratio.
//
// Linux answers through the window system (platform::window_hdr, SDL's window HDR properties). On Wayland that is the
// surface's preferred image description of the colour-management protocol: no absolute levels, only the headroom, the
// display's peak as a multiple of its SDR white. The compositor shows an HDR10 surface's reference white (203 nits, the
// protocol's level for ST 2084 content that declares none, as the Vulkan WSI's does not) at the brightness the user set
// for SDR content, so paper white is those 203 nits and the peak is 203 times the headroom, in the signal's own nits.
// X11 has no such answer: HDR reads as off there.
//
// gamescope (Steam's game mode on a handheld, or nested on a desktop) is asked first. Its Vulkan layer is active exactly when
// GAMESCOPE_WAYLAND_DISPLAY is set, and it adds the HDR10 and scRGB formats to an X11 surface's list only while gamescope's
// HDR output is on (a Wayland surface never gets them), so there the format list is the answer (formats_tell). gamescope
// gives the application no luminance numbers: paper white is the 203 nits, the peak is left unknown.
#include "extras/win_display.h"

#if !defined(_WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#else
#include "nxrt/platform.h"
#endif

namespace nxrt::extras {

bool query_display_hdr(void* hwnd, DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
#if defined(__APPLE__)
    (void)hwnd;
    // The main display is the one a single-window game is on; a second monitor's headroom would need the NSScreen the
    // window is on, which SDL exposes only as the window's display index.
    CGDirectDisplayID display = CGMainDisplayID();
    out->valid = true;

    if (CGDisplayIsBuiltin(display)) {
        snprintf(out->monitor, sizeof out->monitor, "builtin");
    } else {
        snprintf(out->monitor, sizeof out->monitor, "display-%u", static_cast<unsigned>(display));
    }

    // A display whose colour space is extended-range is one the compositor lets exceed SDR white, which is what "HDR
    // on" means on macOS. Plain Display P3 is bounded (SDR wide gamut), so it does not read as extended.
    // CGDisplayModeCopyPixelEncoding, the obvious query, was deprecated and stops answering in 10.11; the EDR headroom
    // itself (NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue) needs AppKit and the window's screen.
    CGColorSpaceRef space = CGDisplayCopyColorSpace(display);
    const bool wide = space && CGColorSpaceUsesExtendedRange(space);
    if (space) CGColorSpaceRelease(space);
    out->hdr_enabled = wide;
    if (wide) {
        // Extended range: the panel can exceed SDR white. The exact headroom is not exposed through CoreGraphics, so
        // max_nits stays 0 ("unknown") and hdr_resolve_levels falls back to its 1000-nit default. vk_renderer.cpp
        // logs these values and uses hdr_enabled for the Auto decision.
        snprintf(out->how, sizeof out->how, "CoreGraphics: extended-range colour space");
    } else {
        snprintf(out->how, sizeof out->how, "CoreGraphics: %s colour space", space ? "bounded" : "no");
    }
    out->min_nits = 0.0f;
    out->sdr_white_nits = 80.0f;
    return true;
#else
    (void)hwnd;
    constexpr float kReferenceWhiteNits = 203.0f;   // ITU-R BT.2408: reference white of ST 2084 content
    if (const char* gs = getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs && *gs) {
        out->valid = true;
        out->formats_tell = true;
        out->sdr_white_nits = kReferenceWhiteNits;
        snprintf(out->monitor, sizeof out->monitor, "of gamescope");
        snprintf(out->how, sizeof out->how, "gamescope: the surface's HDR formats are the answer");
        return true;
    }
    platform::WindowHdr wh;
    if (!platform::window_hdr(&wh)) {
        snprintf(out->how, sizeof out->how, "no window");
        return false;
    }
    out->valid = true;
    out->hdr_enabled = wh.enabled;
    snprintf(out->monitor, sizeof out->monitor, "of the window");
    if (wh.enabled) {
        out->sdr_white_nits = kReferenceWhiteNits;
        out->max_nits = kReferenceWhiteNits * wh.headroom;
    }
    snprintf(out->how, sizeof out->how, "window system: headroom %.2f", static_cast<double>(wh.headroom));
    return true;
#endif
}

}  // namespace nxrt::extras

#endif  // !_WIN32
