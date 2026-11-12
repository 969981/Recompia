# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt: game-independent runtime (OS/HLE layer, graphics shell) for static recompilations of Switch games.
# Build: scripts\build.ps1   (clang-cl + Ninja, Release)
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(nxrt C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
    message(FATAL_ERROR "nxrt targets native Windows x64 only")
endif()

# The generated-code ABI header (Ctx, call_indirect, ...) lives in the recompiler (recomp/include/nxrecomp/).
# Found next to the checkout (../recomp) or, for a git worktree under runtime-wt/<name>, two levels up (../../recomp).
set(NXRT_RECOMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/include" CACHE PATH "Directory that holds nxrecomp/")
if(NOT EXISTS "${NXRT_RECOMP_INCLUDE}/nxrecomp/ctx.h" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include/nxrecomp/ctx.h")
    get_filename_component(_nxrt_ri "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include" ABSOLUTE)
    set(NXRT_RECOMP_INCLUDE "${_nxrt_ri}" CACHE PATH "Directory that holds nxrecomp/" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(/W3 -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch
                        -Wno-cast-function-type-strict -Wno-language-extension-token)
endif()
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)

option(NXRT_BUILD_GFX "Build the graphics shell (SDL3 + Vulkan + NVN front end, src/gfx, cmake/gfx.cmake)" ON)
# Game modules (games/<id>/, docs/GAME_BOUNDARY.md): the title-specific code a game exe links.
option(NXRT_BUILD_GAMES "Build the game modules in games/" ON)
set(NXRT_GAMES "pkp" CACHE STRING "ids of the game modules to build (games/<id>/), a list")
# Test tooling (src/testtools, include/nxrt/tt.h: deterministic mode, scripted runs, traces, input record and replay). Off: the runtime
# builds without it and src/core/tt_off.cpp answers its entry points (everything off). Release packages turn it off (game-build: PKP_RELEASE).
# Code outside the runtime library tests the compile definition NXRT_TESTTOOLS (0 or 1).
option(NXRT_TESTTOOLS "Build the test tooling (src/testtools)" ON)

file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/gfx/")   # the graphics shell is its own library (cmake/gfx.cmake)
if(NXRT_TESTTOOLS)
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/core/tt_off[.]cpp$")
else()
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/testtools/")
endif()
add_library(nxrt STATIC ${NXRT_SOURCES})
target_include_directories(nxrt PUBLIC include)
target_compile_definitions(nxrt PUBLIC NXRT_TESTTOOLS=$<BOOL:${NXRT_TESTTOOLS}>)
if(EXISTS "${NXRT_RECOMP_INCLUDE}")
    target_include_directories(nxrt PUBLIC "${NXRT_RECOMP_INCLUDE}")
endif()
target_link_libraries(nxrt PUBLIC psapi)
add_subdirectory(third_party/pokopia-coop)   # [net] protocol = native: pokopia-native's Link Play (MIT, C11)
target_link_libraries(nxrt PRIVATE pokopia_coop)
add_subdirectory(third_party/miniz)   # zlib streams (MIT): the Pokopia save repair, games/pkp/core/pkp_save_uuid.cpp
add_subdirectory(third_party/modtopia)   # the Modtopia mod loader core (MIT, C11): the mods host, src/mods/ (nxrt/mods.h)
target_link_libraries(nxrt PUBLIC nxrt_modtopia PRIVATE nxrt_modtopia_mappings)
include(cmake/media_deps.cmake)   # libvpx for the Switch 2 video decoders (optional)

if(NXRT_BUILD_GFX)
    include(cmake/gfx.cmake)
endif()

if(NXRT_BUILD_GAMES)
    foreach(_game ${NXRT_GAMES})
        add_subdirectory(games/${_game})
    endforeach()
endif()

# Developer tool, in no package: the controls.ini docs (docs/CONFIG.md) and the launcher's schema (docs/controls_schema.json) written from
# the settings table with one game's factory defaults (its data library, docs/GAME_BOUNDARY.md). scripts/gen_settings_doc.ps1 runs it.
if(NXRT_BUILD_GFX AND NXRT_BUILD_GAMES)
    foreach(_game ${NXRT_GAMES})
        if(TARGET nxrt_game_${_game}_data)
            add_executable(nxrt_settings_doc_${_game} EXCLUDE_FROM_ALL apps/settings_doc.cpp)
            target_link_libraries(nxrt_settings_doc_${_game} PRIVATE nxrt_gfx nxrt_game_${_game}_data)
            target_compile_definitions(nxrt_settings_doc_${_game} PRIVATE NXRT_SETTINGS_DOC_REGISTER=nxrt_register_game_${_game}_data)
        endif()
    endforeach()
endif()
