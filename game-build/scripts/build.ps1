# Configure + build pkp.exe into game-build\build\release (runtime sources are compiled in; the game libraries are imported).
param([string]$Config = "Release", [switch]$Clean, [string]$Runtime = $env:PKP_RUNTIME, [string]$Out = $env:PKP_BUILD_DIR, [string]$Version = "dev", [string]$LibDir = $env:PKP_LIB_DIR)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bd = if ($Out) { $Out } else { Join-Path $root ("build\" + $Config.ToLower()) }
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
$env_cmd = Join-Path $root "..\runtime\scripts\env.cmd"
$cfg = "cmake -S `"$root`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DPKP_VERSION=$Version"
if ($Runtime) { $cfg += " -DPKP_RUNTIME_DIR=`"$Runtime`"" }
if ($LibDir) { $cfg += " -DPKP_LIB_DIR=`"$LibDir`"" }
cmd /c "call `"$env_cmd`" && $cfg && cmake --build `"$bd`" --target pkp"
exit $LASTEXITCODE
