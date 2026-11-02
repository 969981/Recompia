// SPDX-License-Identifier: GPL-3.0-or-later
// The one call between pkp.exe (linked on the player's PC: the lifted game libraries + pkp_stub.cpp) and pkp_rt.dll (the runtime,
// shipped prebuilt). Plain C, so the two sides may come from different compilers (zig/clang mingw for the exe, clang-cl for the DLL).
// Everything else the lifted code calls (recomp ctx.h: call_indirect, host_import, guest_svc, ...) is exported by pkp_rt.dll as well
// (src/pkp_rt.def).
#pragma once
#include <stdint.h>

#define PKP_RT_API 1u     // bump when this struct or the exported set changes

#ifdef __cplusplus
extern "C" {
#endif
struct NxModule;
typedef struct PkpRtModules {
    uint32_t size;        // sizeof(PkpRtModules)
    uint32_t api;         // PKP_RT_API
    const struct NxModule *rtld, *main, *sdk, *subsdk0;
    const uint32_t *rtld_abi, *main_abi, *sdk_abi, *subsdk0_abi;
} PkpRtModules;

int pkp_rt_main(int argc, char** argv, const PkpRtModules* mods);
#ifdef __cplusplus
}
#endif
