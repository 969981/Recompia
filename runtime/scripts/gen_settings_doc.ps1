# Writes the generated controls.ini part of docs\CONFIG.md and docs\controls_schema.json (the launcher's schema) from the settings table
# (src\gfx\play\play_config_table.cpp), with one game's factory defaults. Run after changing the table and commit both files.
#   pwsh scripts\gen_settings_doc.ps1 [-BuildDir <a configured runtime or game build>] [-Game pkp]
# BuildDir defaults to <checkout>\build\release (scripts\build.ps1); a game-build folder (game-build\build\exe) works too.
param([string]$BuildDir = "", [string]$Game = "pkp")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\release" }
$target = "nxrt_settings_doc_$Game"
cmd /c "call `"$(Join-Path $PSScriptRoot 'env.cmd')`" && cmake --build `"$BuildDir`" --target $target"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$exe = Get-ChildItem -Path $BuildDir -Recurse -Filter "$target.exe" | Select-Object -First 1
if (-not $exe) { Write-Error "$target.exe not found under $BuildDir" }
& $exe.FullName --markdown (Join-Path $root "docs\CONFIG.md") --schema (Join-Path $root "docs\controls_schema.json")
exit $LASTEXITCODE
