# Writes the generated parts of docs\CONFIG.md (the controls.ini keys and the game ini's [<game>] keys) and the launcher's schemas
# docs\controls_schema.json and docs\<game>_ini_schema.json from the settings tables (src\gfx\play\play_config_table.cpp and the game
# module's, game::Info::settings_doc), with one game's factory defaults. Run after changing a table and commit the files.
#   pwsh scripts\gen_settings_doc.ps1 [-BuildDir <a configured runtime or game build>] [-Game pkp] [-Check]
# BuildDir defaults to <checkout>\build\release (scripts\build.ps1); a game-build folder (game-build\build\exe) works too.
# -Check writes nothing and exits 2 when a generated file is out of date (it names each one): run it before a pull request.
param([string]$BuildDir = "", [string]$Game = "pkp", [switch]$Check)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\release" }
$target = "nxrt_settings_doc_$Game"
cmd /c "call `"$(Join-Path $PSScriptRoot 'env.cmd')`" && cmake --build `"$BuildDir`" --target $target"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$exe = Get-ChildItem -Path $BuildDir -Recurse -Filter "$target.exe" | Select-Object -First 1
if (-not $exe) { Write-Error "$target.exe not found under $BuildDir" }
$docs = Join-Path $root "docs"
$toolArgs = @(
    "--markdown", (Join-Path $docs "CONFIG.md"),
    "--schema", (Join-Path $docs "controls_schema.json"),
    "--game-schema", (Join-Path $docs "${Game}_ini_schema.json"))
if ($Check) { $toolArgs = @("--check") + $toolArgs }
& $exe.FullName @toolArgs
if ($Check -and $LASTEXITCODE -eq 2) { Write-Host "Generated settings docs are out of date: run scripts\gen_settings_doc.ps1 and commit the result." }
exit $LASTEXITCODE
