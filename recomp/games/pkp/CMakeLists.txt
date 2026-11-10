# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pokémon Pokopia: lift the NSOs of the user's dump and build one static library per module.
#
#   scripts\build_game.cmd pkp                  (configure + build into work\build\pkp; NX_ROOT is the repository root)
#
# Inputs (read-only): <root>\game\dump\exefs\{main,sdk,subsdk0,rtld}, or the newest <root>\game\dump\updates\<version>\exefs (the
# launcher's dump layout). Generated C++ goes to <root>\work\lift\<module>, build output to
# work\build\pkp: neither is ever committed. Targets: pkp_main, pkp_sdk, pkp_subsdk0, pkp_rtld (static libraries, each provides
# `nxmod_<name>`: ctx.h NxModule), and `pkp_modules` builds all four.
cmake_minimum_required(VERSION 3.25)
project(pkp_lift C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NxRecomp.cmake")

get_filename_component(PKP_ROOT_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)   # the repository root: recomp/games/pkp/../../..
set(PKP_ROOT "${PKP_ROOT_DEFAULT}" CACHE PATH "repository root (game/, work/)")
if(DEFINED NX_ROOT)   # scripts\build_game.cmd passes the repository root
    get_filename_component(PKP_ROOT "${NX_ROOT}" ABSOLUTE)
    set(PKP_ROOT "${PKP_ROOT}" CACHE PATH "repository root (game/, work/)" FORCE)
endif()
set(PKP_EXEFS_DEFAULT "${PKP_ROOT}/game/dump/exefs")
if(NOT EXISTS "${PKP_EXEFS_DEFAULT}/main")   # the launcher's layout: the newest updates/<version>/exefs
    file(GLOB _pkp_updates LIST_DIRECTORIES true "${PKP_ROOT}/game/dump/updates/*/exefs")
    list(SORT _pkp_updates COMPARE NATURAL ORDER DESCENDING)
    if(_pkp_updates)
        list(GET _pkp_updates 0 PKP_EXEFS_DEFAULT)
    endif()
endif()
set(PKP_EXEFS "${PKP_EXEFS_DEFAULT}" CACHE PATH "folder with the NSOs main, sdk, subsdk0, rtld")
message(STATUS "pkp: lifting the NSOs in ${PKP_EXEFS}")
set(PKP_GEN "${PKP_ROOT}/work/lift" CACHE PATH "where the generated sources go")
# the module table (names, exefs files, bases, NxModule.index, translation units, site lists) is recomp/games/pkp/config.py,
# shared with the player build (game-build/release_b/pkp_build.py)
nxrecomp_build_info(pkp PKP_MOD)
set(PKP_SITES "${PKP_MOD_main_SITES}" CACHE FILEPATH "hook sites of main")

set(_pkp_targets "")
foreach(m IN LISTS PKP_MOD_MODULES)
    set(_sites "${PKP_MOD_${m}_SITES}")
    if(m STREQUAL "main")
        set(_sites "${PKP_SITES}")
    endif()
    set(_sites_arg "")
    if(_sites)
        set(_sites_arg SITES "${_sites}")
    endif()
    nxrecomp_add_module(pkp_${m} INPUT "${PKP_EXEFS}/${PKP_MOD_${m}_FILE}" NAME ${m} INDEX ${PKP_MOD_${m}_INDEX}
                        BASE ${PKP_MOD_${m}_BASE} GAME pkp SHARDS ${PKP_MOD_${m}_SHARDS} ${_sites_arg} OUTDIR "${PKP_GEN}/${m}")
    list(APPEND _pkp_targets pkp_${m})
endforeach()
add_custom_target(pkp_modules DEPENDS ${_pkp_targets})
