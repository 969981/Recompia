# SPDX-License-Identifier: GPL-3.0-or-later
# The site-list stamp of a lifted module (NxRecomp.cmake): which hook sites a module library was lifted with, so a tree that links
# imported libraries can tell they came from another site list (a module lifted before a site was added never calls that hook).
#   cmake -DMODE=write -DSITES=<site list or empty> -DOUT=<stamp> -P nx_sites_stamp.cmake
#   cmake -DMODE=check -DSITES=<site list or empty> -DSTAMP=<stamp> -DMODULE=<name> -P nx_sites_stamp.cmake
# The stamp holds the SHA-256 of the site list with CRLF read as LF (checkouts differ in line endings), or "none", then the list's path.
cmake_minimum_required(VERSION 3.25)

function(_nx_sites_hash out)
  if(SITES)
    file(READ "${SITES}" text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(SHA256 h "${text}")
  else()
    set(h none)
  endif()
  set(${out} "${h}" PARENT_SCOPE)
endfunction()

_nx_sites_hash(hash)
if(MODE STREQUAL "write")
  file(WRITE "${OUT}" "${hash} ${SITES}\n")
elseif(MODE STREQUAL "check")
  if(NOT EXISTS "${STAMP}")
    message(FATAL_ERROR "lifted module ${MODULE}: no site stamp ${STAMP}: the libraries were lifted before stamps existed or "
                        "elsewhere. Lift them again from this tree (recomp/scripts/build_game.cmd).")
  endif()
  file(READ "${STAMP}" stamp)
  string(STRIP "${stamp}" stamp)
  string(REGEX MATCH "^[^ ]+" was "${stamp}")
  string(REGEX REPLACE "^[^ ]+ ?" "" was_path "${stamp}")
  if(NOT was STREQUAL hash)
    if(NOT was_path)
      set(was_path "no site list")
    endif()
    if(NOT SITES)
      set(SITES "no site list")
    endif()
    message(FATAL_ERROR "lifted module ${MODULE} was lifted with other hook sites (${was_path}) than this tree's (${SITES}): "
                        "hooks on the missing sites would never run. Lift again (recomp/scripts/build_game.cmd) or point the "
                        "library folder at a lift of this tree.")
  endif()
else()
  message(FATAL_ERROR "nx_sites_stamp.cmake: MODE must be write or check")
endif()
