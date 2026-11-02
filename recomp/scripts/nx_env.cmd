@echo off
rem Sets up the build environment: MSVC x64 (vcvars64), LLVM clang-cl 23, CMake, Ninja, sccache.
rem Usage from cmd:  scripts\nx_env.cmd cmake --build build\x
rem                  scripts\nx_env.cmd            (no arguments: just prints the tool versions)
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%~dp0..\..\tools\.venv\Scripts;%PATH%"
if "%~1"=="" (
  clang-cl --version & cmake --version & ninja --version & sccache --version
  exit /b 0
)
%*
