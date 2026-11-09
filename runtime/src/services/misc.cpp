// SPDX-License-Identifier: GPL-3.0-or-later
// Small Horizon services of the title's Service Access Control list (S3c): csrng, ldr:ro, mm:u, hwopus, aoc:u, caps:u, caps:su,
// mii:u, irs, nfc:user, nfc:mf:u, nfp:user, ectx:aw, pcm, banana, htc, htc:tenv, htcs, olsc:u.
//
// Adapted from ScarletNX src/nx/hle/services/misc.swipc, ldr.swipc and misc.c (GPL-3.0-or-later): the command ids and argument
// layouts of csrng, mm:u, aoc:u, irs, mii:u, nfc/nfp, ectx:aw and ldr:ro, and the no-device result constants.
// Cross-checked against Switchbrew (https://switchbrew.org/wiki/Services_API and the per-service pages: RO services, SPL services,
// Audio services (hwopus), Capture services, NFC services, TMA services, Error_codes), and Ryubing (MIT) src/Ryujinx.HLE/HOS/Services
// (Ro/IRoInterface.cs, Spl/IRandomInterface.cs, Caps/*, Ectx/*, Ptm/Pcm/IManager.cs, Ns/Aoc/IAddOnContentManager.cs, Hid/Irs/
// IIrSensorServer.cs, Mii/StaticService/IDatabaseService.cs, Nfc/*).
//
// Every command in a table has a handler: accepted-and-ignored (logged at Debug), an error result, or a real answer. No table command
// falls through to kStubService. Two services have no table in the sources read, so their commands do fall through: pcm, and the IUser
// that nfc:mf:u hands out. "banana" is a StubService by design.
//
// The services are in misc_*.cpp (one file per group, misc_internal.h has what they share); this file registers them and resets their state.

#include "misc_internal.h"

namespace nxrt::services {
using namespace miscsvc;

// ---- registration ------------------------------------------------------------------------------------------------------------------

void register_misc() {
    add("stpl:u", [] { return make_streamplay_user(); });
    add("csrng", [] { return make_csrng(); });
    add("ldr:ro", [] { return make_ro(); });
    add("mm:u", [] { return make_mm(); });
    add("hwopus", [] { return make_hwopus(); });
    add("aoc:u", [] { return make_aoc(); });
    add("caps:u", [] { return make_album_app(); });
    add("caps:su", [] { return make_screenshot_app(); });
    add("mii:u", [] { return make_mii_static(); });
    add("irs", [] { return make_irs(); });
    add("nfc:user", [] { return make_user_manager("nn::nfc::detail::IUserManager", &make_nfc_user); });
    add("nfc:mf:u", [] { return make_user_manager("nn::nfc::mifare::detail::IUserManager", &make_mifare_user); });
    add("nfp:user", [] { return make_user_manager("nn::nfp::detail::IUserManager", &make_nfp_user); });
    add("ectx:aw", [] { return make_ectx(); });
    add("pcm", [] { return make_iface("pcm", [](Service&) {}); });
    add("banana", [] { return std::make_shared<BananaService>(); });
    add("htc", [] { return make_tma("nn::tma::IHtcManager", htc_cmds()); });
    add("htc:tenv", [] { return make_tma("nn::htc::tenv::IServiceManager", htc_tenv_cmds()); });
    add("htcs", [] { return make_tma("nn::tma::IHtcsManager", htcs_cmds()); });
    add("olsc:u", [] { return make_olsc(); });
}

// Process-wide state back to its start-up value: the clock requests, and the events (cleared).
void misc_reset() {
    {
        MmState& s = mm_state();
        std::lock_guard<std::mutex> g(s.mu);
        s.next_id = 0;
        for (auto& sl : s.slots) sl = MmSlot{};
    }
    MiscEvents& e = events();
    e.aoc_list_changed.clear();
    e.aoc_purchased.clear();
    e.nfc_available.clear();
    e.nfc_activate.clear();
    e.nfc_deactivate.clear();
    e.nfp_available.clear();
    e.nfp_activate.clear();
    e.nfp_deactivate.clear();
}

}  // namespace nxrt::services
