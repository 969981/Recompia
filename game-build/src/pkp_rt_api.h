// SPDX-License-Identifier: GPL-3.0-or-later
// The one call between pkp.exe (linked on the player's PC: the lifted game libraries + pkp_stub.cpp) and pkp_rt.dll (the runtime,
// shipped prebuilt). Plain C, so the two sides may come from different compilers (zig/clang mingw for the exe, clang-cl for the DLL).
// Everything else the lifted code calls (recomp ctx.h: call_indirect, host_import, guest_svc, ...) is exported by pkp_rt.dll as well
// (src/pkp_rt.def).
#pragma once
#include <stdint.h>

#define PKP_RT_API 2u     // bump when this struct or the exported set changes
// Both sides export this number as the data symbol pkp_rt_api (pkp_stub.cpp; pkp_main.cpp through pkp_rt.def), so the launcher
// reads from each file's export table which API it was built for, before the game starts (launcher/src-tauri/src/runtime_api.rs,
// which also reads the define above as text from the shipped copy of this header).

#ifdef __cplusplus
extern "C" {
#endif
struct NxModule;
typedef struct PkpRtModules {
    uint32_t size;        // sizeof(PkpRtModules)
    uint32_t api;         // PKP_RT_API
    uint32_t count;       // modules in the two arrays below, in load order (nx_modules.gen.h, written from the game config)
    const struct NxModule* const* mods;       // nxmod_<name> of each module
    const uint32_t* const* abis;              // nxmod_<name>_abi of each module
} PkpRtModules;

int pkp_rt_main(int argc, char** argv, const PkpRtModules* mods);
#ifdef __cplusplus
}
#endif
