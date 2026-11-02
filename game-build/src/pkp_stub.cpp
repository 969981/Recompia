// SPDX-License-Identifier: GPL-3.0-or-later
// pkp.exe of release model (b): built on the player's PC (tools/build/pkp_build.py, bundled Zig) from this file and the four libraries
// lifted out of their own dump. All it does is hand the lifted module tables to the runtime in pkp_rt.dll (pkp_rt_api.h).
#include <stdint.h>
#include "pkp_rt_api.h"

extern "C" {
extern const struct NxModule nxmod_main, nxmod_sdk, nxmod_subsdk0, nxmod_rtld;
extern const uint32_t nxmod_main_abi, nxmod_sdk_abi, nxmod_subsdk0_abi, nxmod_rtld_abi;
}

// The manifest (res/pkp.manifest) sets the UTF-8 code page, so argv is UTF-8 like the runtime expects.
int main(int argc, char** argv) {
    static const PkpRtModules mods = {
        sizeof(PkpRtModules), PKP_RT_API,
        &nxmod_rtld, &nxmod_main, &nxmod_sdk, &nxmod_subsdk0,
        &nxmod_rtld_abi, &nxmod_main_abi, &nxmod_sdk_abi, &nxmod_subsdk0_abi,
    };
    return pkp_rt_main(argc, argv, &mods);
}
