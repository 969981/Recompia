@echo off
rem Sets up the build environment: MSVC x64 (vcvars64), LLVM clang-cl 23, CMake, Ninja, sccache (tools\scripts\msvc_env.cmd),
rem plus the recompiler's Python (tools\.venv) on PATH.
rem Usage from cmd:  scripts\nx_env.cmd cmake --build build\x
rem                  scripts\nx_env.cmd            (no arguments: just prints the tool versions)
rem                  call scripts\nx_env.cmd --quiet   (only sets up the environment of the calling shell)
call "%~dp0..\..\tools\scripts\msvc_env.cmd"
set "PATH=%~dp0..\..\tools\.venv\Scripts;%PATH%"
if "%~1"=="--quiet" exit /b 0
if "%~1"=="" (
  clang-cl --version & cmake --version & ninja --version & sccache --version
  exit /b 0
)
%*
