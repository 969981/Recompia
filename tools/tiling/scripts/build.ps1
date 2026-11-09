# Build tegra_tiling (library, tegra_tile CLI) with CMake + Ninja + clang-cl into tools\tiling\build\release.
#   pwsh -File tools\tiling\scripts\build.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $root 'build\release'
. (Join-Path $PSScriptRoot '..\..\scripts\msvc.ps1')
$vcvars = Get-VcVars64
Import-MsvcEnv $vcvars
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $build
if ($LASTEXITCODE) { throw 'build failed' }
Write-Output "built: $build"
