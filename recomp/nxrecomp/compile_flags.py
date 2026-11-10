# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile flags of the lifted code: the one table for every build of it.

    cmake/NxRecomp.cmake (developer build, clang-cl)   CLANG_CL_OPT + CLANG_CL     (NX_OPT replaces CLANG_CL_OPT)
    cmake/NxRecomp.cmake with a non-MSVC compiler       CLANG
    game-build/release_b/pkp_build.py (player build)    ZIG_MINGW                   (zig c++, x86_64-windows-gnu)

CMake reads them through `python -m nxrecomp build-info`. The lists are not identical, and the differences are kept as they
were when the tables were merged (the player build and the developer build each produce working objects with their own):
  * optimisation: clang-cl /O1 is clang's -Os (size), the player build uses -O1;
  * the player build adds -fno-threadsafe-statics (clang-cl keeps the MSVC thread-safe local statics);
  * target: the developer build is the MSVC ABI (static CRT /MT from cmake/clang-cl.cmake), the player build mingw;
    -mcx16 and -mcpu=x86_64+cx16 both turn on cmpxchg16b on the x86-64 baseline.
Changing ZIG_MINGW changes the players' build stamp and object cache keys (every unit compiles again).
"""

# what the generated code's meaning depends on: guest memory is untyped (no type-based alias analysis), and lifted FP must
# not be fused or reordered (smo-recomp's flags)
SEMANTIC = ['-fno-strict-aliasing', '-ffp-contract=off']

CLANG_CL_OPT = '/O1'
CLANG_CL = ['/w', '/EHs-c-', '/GR-'] + SEMANTIC + ['-mcx16']
CLANG = ['-O1', '-w', '-fno-exceptions', '-fno-rtti'] + SEMANTIC + ['-mcx16']
ZIG_MINGW = (['-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-std=c++20', '-O1', '-w', '-fno-exceptions', '-fno-rtti']
             + SEMANTIC + ['-fno-threadsafe-statics'])
