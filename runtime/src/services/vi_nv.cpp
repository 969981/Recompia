// SPDX-License-Identifier: GPL-3.0-or-later
// vi:u, vi:s, vi:m (the display services and the binder driver of a layer's buffer queue) and nvdrv, nvdrv:a, nvdrv:s, nvdrv:t (the
// nvhost devices), as far as the SDK uses them before NVN is handed to our graphics bootstrap (S3c).
//
// Adapted from ScarletNX src/nx/hle/services/vi.swipc, vi.c, nvdrv.swipc and nvdrv.c (GPL-3.0-or-later; vi.c takes its parcel and
// buffer-queue layouts from Eden and yuzu, GPL-2.0-or-later) and from Ryujinx src/Ryujinx.HLE/HOS/Services/{Vi,Nv,SurfaceFlinger}
// (MIT). Command ids and layouts were also checked against https://switchbrew.org/wiki/NV_services and Display_services.
//
// Not here, left to the GPU back end: nvhost channel submission (SUBMIT_GPFIFO only answers success), fences beyond success, presentation.
//
// The services are in vi_display.cpp (vi), vi_nvdrv.cpp (nvdrv) and vi_nv_common.cpp (helpers both use); this file registers them and resets their state.

#include "vi_nv_internal.h"

namespace nxrt::services {
using namespace vinv;

// ============================================================ registration

void register_vi_nv() {
    add("vi:u", [] { return make_iface("nn::visrv::sf::IApplicationRootService", setup_vi_u); });
    add("vi:s", [] { return make_iface("nn::visrv::sf::ISystemRootService", setup_vi_s); });
    add("vi:m", [] { return make_iface("nn::visrv::sf::IManagerRootService", setup_vi_m); });
    for (const char* name : {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"})
        add(name, [] { return make_iface("nns::nvdrv::INvDrvServices", setup_nvdrv); });
}

void vi_nv_reset() {
    vsync_stop_and_join();
    {
        ViState& s = vi();
        std::lock_guard<std::mutex> g(s.mu);
        for (Layer& l : s.layers) {
            l.used = false;
            l.connected = false;
            l.layer_id = 0;
            l.event = ServiceEvent();
        }
        s.next_stray = kStrayLayerBase;
        s.vsync = ServiceEvent();
    }
    {
        NvState& n = nv();
        std::lock_guard<std::mutex> g(n.mu);
        n.initialized = false;
        n.next_fd = 1;
        n.next_handle = 1;
        n.next_syncpt = 1;
        n.files.clear();
        n.maps.clear();
        n.seen.clear();
        n.warned.clear();
        n.submit_logged = false;
    }
}

namespace viapi {
uint32_t display_vsync_handle() { return vsync_new_handle(); }
}  // namespace viapi

}  // namespace nxrt::services
