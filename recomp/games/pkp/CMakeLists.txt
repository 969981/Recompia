# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pokémon Pokopia: lift the NSOs of the user's dump and build one static library per module.
#
#   scripts\build_game.cmd pkp                  (configure + build into work\build\pkp; NX_ROOT is the repository root)
#
# Inputs (read-only): <root>\game\exefs\{main,sdk,subsdk0,rtld}. Generated C++ goes to <root>\work\lift\<module>, build output to
# work\build\pkp: neither is ever committed. Targets: pkp_main, pkp_sdk, pkp_subsdk0, pkp_rtld (static libraries, each provides
# `nxmod_<name>`: ctx.h NxModule), and `pkp_modules` builds all four.
cmake_minimum_required(VERSION 3.25)
project(pkp_lift C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NxRecomp.cmake")

get_filename_component(PKP_ROOT_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)   # the project root (nx/ is the engine clone inside it)
set(PKP_ROOT "${PKP_ROOT_DEFAULT}" CACHE PATH "repository root (game/, work/)")
if(DEFINED NX_ROOT)
    get_filename_component(PKP_ROOT "${NX_ROOT}/.." ABSOLUTE)
    set(PKP_ROOT "${PKP_ROOT}" CACHE PATH "project root (game/, work/)" FORCE)
endif()
set(PKP_EXEFS "${PKP_ROOT}/game/dump/exefs" CACHE PATH "folder with the NSOs main, sdk, subsdk0, rtld")
set(PKP_GEN "${PKP_ROOT}/work/lift" CACHE PATH "where the generated sources go")
# translation units per module: about 45k instructions each (docs/S1b.md); main has 10.3 M instruction words
set(PKP_SHARDS_MAIN 230 CACHE STRING "translation units of main")
set(PKP_SHARDS_SDK 45 CACHE STRING "translation units of sdk")
set(PKP_SHARDS_SUBSDK0 4 CACHE STRING "translation units of subsdk0")
set(PKP_SITES "${CMAKE_CURRENT_LIST_DIR}/main.sites" CACHE FILEPATH "hook sites of main for this build (resolved from sites.sig)")

nxrecomp_add_module(pkp_main INPUT "${PKP_EXEFS}/main" NAME main INDEX 0 BASE 0x7100000000 GAME pkp SHARDS ${PKP_SHARDS_MAIN}
                    SITES "${PKP_SITES}" OUTDIR "${PKP_GEN}/main")
nxrecomp_add_module(pkp_sdk INPUT "${PKP_EXEFS}/sdk" NAME sdk INDEX 1 BASE 0x7500000000 GAME pkp SHARDS ${PKP_SHARDS_SDK}
                    OUTDIR "${PKP_GEN}/sdk")
nxrecomp_add_module(pkp_subsdk0 INPUT "${PKP_EXEFS}/subsdk0" NAME subsdk0 INDEX 2 BASE 0x7600000000 GAME pkp SHARDS ${PKP_SHARDS_SUBSDK0}
                    OUTDIR "${PKP_GEN}/subsdk0")
nxrecomp_add_module(pkp_rtld INPUT "${PKP_EXEFS}/rtld" NAME rtld INDEX 3 BASE 0x7700000000 GAME pkp SHARDS 1
                    OUTDIR "${PKP_GEN}/rtld")
add_custom_target(pkp_modules DEPENDS pkp_main pkp_sdk pkp_subsdk0 pkp_rtld)

# Link check (target pkp_link_check): the four libraries plus stubs for what the runtime provides. Proves every symbol resolves.
add_executable(pkp_link_check EXCLUDE_FROM_ALL ../mpr/link_check.cpp)
target_link_libraries(pkp_link_check PRIVATE pkp_main pkp_sdk pkp_subsdk0 pkp_rtld)
target_compile_options(pkp_link_check PRIVATE /w /EHs-c- /GR-)
