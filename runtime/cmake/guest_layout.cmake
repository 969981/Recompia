# SPDX-License-Identifier: GPL-3.0-or-later
# The guest address space's base on the target (include/nxrt/memory.h places every region at a fixed offset from it). Not set:
# the canonical 0x7000000000 that every site list, config and document is written for, and nothing below does anything. A target
# that needs another base sets NXRT_GUEST_BASE here and nowhere else (no current target does). Its readers: the runtime (a compile
# definition), the developer lifts and module header (NX_GUEST_BASE, recomp/cmake/NxRecomp.cmake, through
# game-build/release_b/guest_layout.py) and the player build (guest_base.txt next to the runtime library, which a kit ships as
# sdk/guest_base.txt for pkp_build.py).
# Included by every project that compiles runtime code or lifts; the first include wins and the directories below inherit it.
include_guard(GLOBAL)
if(DEFINED NXRT_GUEST_BASE)
    add_compile_definitions(NXRT_GUEST_BASE=${NXRT_GUEST_BASE}ull)
    set(NX_GUEST_BASE ${NXRT_GUEST_BASE})
endif()
