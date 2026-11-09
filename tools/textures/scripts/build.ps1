# Configure + build astc2bc7 (library, CLI, astcenc decoder DLLs) with CMake + Ninja + clang-cl.
#   pwsh -File tools\textures\scripts\build.ps1 [-Clean]
# Needs third_party\ (see third_party\VERSIONS.md): astc-encoder, bc7enc_rdo, ispc. Output: tools\textures\build\release\
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $root 'build\release'
. (Join-Path $PSScriptRoot '..\..\scripts\msvc.ps1')
$vcvars = Get-VcVars64
Import-MsvcEnv $vcvars
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $build
if ($LASTEXITCODE) { throw 'build failed' }
Write-Output "built: $build\astc2bc7.exe"
