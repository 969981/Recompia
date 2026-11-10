@echo off
rem Shared by runtime\scripts\env.cmd and recomp\scripts\nx_env.cmd (call it): the MSVC x64 environment (vcvars64.bat of the
rem newest Visual Studio or Build Tools with the x64 C++ tools, 2022 or newer), then LLVM, CMake and the WinGet links (Ninja,
rem sccache) first on PATH. tools\scripts\msvc.ps1 is the PowerShell counterpart.
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
