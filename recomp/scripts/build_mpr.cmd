@echo off
rem Metroid Prime Remastered: configure and build the three module libraries (work\build\mpr), sccache kept in work\build\sccache.
rem   scripts\build_mpr.cmd [extra ninja args, e.g. -j 20 | -k 0 | mpr_sdk]
rem Environment: MPR_JOBS (default 20) = compile processes; MPR_BUILD (default <root>\work\build\mpr).
rem The same as scripts\build_game.cmd mpr, kept for the old names.
setlocal
if defined MPR_BUILD set "NX_BUILD=%MPR_BUILD%"
if defined MPR_JOBS set "NX_JOBS=%MPR_JOBS%"
call "%~dp0build_game.cmd" mpr %*
exit /b %errorlevel%
