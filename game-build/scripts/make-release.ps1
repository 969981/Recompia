# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the player zip (release model b: no game code or data in it; the player's PC lifts and links pkp.exe from their own dump).
# Everything built from this repo is taken from the given build outputs and the checkout; the unchanged third-party payload (Zig, embedded
# Python, FSR 3 / Streamline DLLs, astcdec, fonts, input icons, licence texts) comes from a base package (the previous release zip; release.yml BASE_REPO / BASE_RELEASE).
# ponytail: base zip for the third-party payload; fetch each piece from its own source when one of them needs a new version.
# Two zips of the same tree (release_names.rs in the launcher): Recompia-<v>-windows-x64.zip, folder Recompia-<v>/ with Recompia.exe
# and the small starter under the old name PokopiaLauncher.exe (launcher/stub), and the old-name copy PokopiaRecomp-<v>-win64.zip,
# folder PokopiaRecomp-<v>/ with the full launcher as PokopiaLauncher.exe and no starter: the only one a launcher from before the rename
# can match and install. The old-name copy ships for three releases after the rename.
#   pwsh game-build\scripts\make-release.ps1 -Version 0.2.0 -Base PokopiaRecomp-0.1.1-win64.zip -RtDir game-build\build\rtdll `
#        -Launcher launcher\src-tauri\target\release\recompia.exe -Stub launcher\stub\target\release\recompia-stub.exe `
#        -Harness work\rel\harness -Crates work\licenses\THIRD_PARTY_CRATES.txt -Out work\rel
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Base,       # previous player zip (third-party payload)
    [Parameter(Mandatory)][string]$RtDir,      # folder with pkp_rt.dll + pkp_rt.lib (game-build, PKP_RT_DLL=ON PKP_RELEASE=ON)
    [Parameter(Mandatory)][string]$Launcher,   # recompia.exe (launcher, npm run tauri build)
    [Parameter(Mandatory)][string]$Stub,       # recompia-stub.exe (launcher/stub, cargo build --release): the starter under the old name
    [Parameter(Mandatory)][string]$Harness,    # self-contained spirv_harness publish folder
    [Parameter(Mandatory)][string]$Crates,     # THIRD_PARTY_CRATES.txt (the release workflow's licenses job: cargo about, launcher/about.hbs)
    [string]$PokoHex,                          # the Save Editor's data tool (build-pokohex.sh --extractor win-x64 <dir>)
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$name = "Recompia-$Version"
$oldName = "PokopiaRecomp-$Version"
$stage = Join-Path $Out 'stage'
$pkg = Join-Path $stage $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# base payload, its top folder renamed
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $Base).Path, $stage)
$top = Get-ChildItem $stage -Directory | Select-Object -First 1
Rename-Item $top.FullName $name

function Put([string]$from, [string]$to) {
    $dst = Join-Path $pkg $to
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -Force $from $dst
}
function Mirror([string]$from, [string]$to, [string[]]$exclude = @()) {
    $dst = Join-Path $pkg $to
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    New-Item -ItemType Directory -Force $dst | Out-Null
    $src = (Resolve-Path $from).Path
    Get-ChildItem $src -Recurse -File | Where-Object {
        $rel = $_.FullName.Substring($src.Length + 1)
        -not ($exclude | Where-Object { $rel -like $_ })
    } | ForEach-Object {
        $t = Join-Path $dst $_.FullName.Substring($src.Length + 1)
        New-Item -ItemType Directory -Force (Split-Path -Parent $t) | Out-Null
        Copy-Item $_.FullName $t
    }
}

# built here
Put (Join-Path $RtDir 'pkp_rt.dll') 'pkp_rt.dll'
Put (Join-Path $RtDir 'pkp_rt.lib') 'tools\build\sdk\pkp_rt.lib'
# libusb 1.0.30, unchanged official build (runtime/cmake/gfx_deps.cmake copies it next to pkp_rt.dll): SDL loads it by name for
# controllers it reaches over USB, so it sits next to pkp.exe, ahead of any other libusb-1.0.dll on PATH.
Put (Join-Path $RtDir 'libusb-1.0.dll') 'libusb-1.0.dll'
Put "$root\runtime\third_party\libusb\COPYING" 'licenses\LICENSE.libusb.LGPL-2.1.txt'
Put $Launcher 'Recompia.exe'
Put $Stub 'PokopiaLauncher.exe'   # over the base zip's old launcher
Mirror $Harness 'tools\spirv_harness' @('*.pdb')
if ($PokoHex) {
    if (-not (Test-Path (Join-Path $PokoHex 'pokohex-extract.exe'))) { throw "no pokohex-extract.exe in $PokoHex" }
    Mirror $PokoHex 'tools\pokohex'
}
# from the checkout
Put "$root\game-build\src\pkp_rt_api.h" 'tools\build\sdk\pkp_rt_api.h'
Put "$root\game-build\src\pkp_stub.cpp" 'tools\build\sdk\pkp_stub.cpp'
Put "$root\game-build\res\pkp.manifest" 'tools\build\sdk\pkp.manifest'
Put "$root\game-build\res\pkp.rc" 'tools\build\sdk\pkp.rc'
foreach ($f in 'pkp_build.py', 'dump_hashes.json', 'fetch_dlss.py') { Put "$root\game-build\release_b\$f" "tools\build\$f" }
# the optimized build (pkp_build.py --optimize); the launcher offers it only with pgo\tools (llvm-profdata, the profile runtime; gamebuild.rs can_optimize), which this package does not ship yet
foreach ($f in 'pkp_pgo.py', 'profile_writer.cpp', 'session.nxs') { Put "$root\game-build\release_b\pgo\$f" "tools\build\pgo\$f" }
function Readme([string]$launcherName) {
    (Get-Content -Raw "$root\game-build\release_b\README.txt").Replace('@VERSION@', $Version).Replace('@LAUNCHER@', $launcherName) | Set-Content -NoNewline (Join-Path $pkg 'README.txt')
}
Readme 'Recompia.exe'
Put "$root\game-build\release_b\BUNDLED_TOOLS.txt" 'licenses\BUNDLED_TOOLS.txt'
# licence texts and notices (the base package's licenses/ keeps what only it carries: Zig, Python, FSR 3, Streamline)
foreach ($d in 'tools\release\licenses', 'tools\shaders\licenses') {
    Get-ChildItem "$root\$d" -File | ForEach-Object { Put $_.FullName "licenses\$($_.Name)" }
}
Put "$root\runtime\THIRD_PARTY_NOTICES.md" 'licenses\THIRD_PARTY_NOTICES.md'
Put "$root\recomp\THIRD_PARTY.md" 'licenses\THIRD_PARTY_RECOMP.md'
Put "$root\CREDITS.md" 'licenses\CREDITS.md'
Put $Crates 'licenses\THIRD_PARTY_CRATES.txt'
Put "$root\launcher\release\Play.bat" 'Play.bat'
Put "$root\LICENSE" 'LICENSE.txt'
# what RecompLauncher reads at the top of a release (its docs/DEVELOPERS.md): which launcher file to start, the saves to back up
Put "$root\game-build\release_b\recomplauncher.json" 'recomplauncher.json'
Put "$root\CHANGELOG.md" 'CHANGELOG.txt'
Put "$root\tools\shaders\ampere\extract_corpus.py" 'tools\shaders\extract_corpus.py'
Mirror "$root\runtime\assets\prompts" 'assets\prompts'   # button prompt icon pack prompt_icons.pak (Kenney CC0, licence in the folder)
Put "$root\recomp\LICENSE" 'tools\build\recomp\LICENSE'
Mirror "$root\recomp\nxrecomp" 'tools\build\recomp\nxrecomp' @('*__pycache__*', '*.pyc')
Mirror "$root\recomp\include\nxrecomp" 'tools\build\recomp\include\nxrecomp'
foreach ($f in 'CMakeLists.txt', 'config.py', 'main.sites') { Put "$root\recomp\games\pkp\$f" "tools\build\recomp\games\pkp\$f" }
# what the package ships under tools\build\recomp: pkp_build.py stamps a build by these files only, so a file an older release
# left in an updated install (an update never removes one) does not change the stamp
$recompDir = Join-Path $pkg 'tools\build\recomp'
$shipped = Get-ChildItem $recompDir -Recurse -File | ForEach-Object { $_.FullName.Substring($recompDir.Length + 1).Replace('\', '/') } | Sort-Object
[IO.File]::WriteAllText((Join-Path $pkg 'tools\build\recomp.files'), (($shipped -join "`n") + "`n"))

# never ship anything from a game dump, a shader cache built from the game, saves or keys
$bad = Get-ChildItem $pkg -Recurse -Force | Where-Object {
    $_.Name -match '\.(nso|nro|nca|nsp|xci|nup|spv|keys|bin)$' -or $_.Name -in 'exefs', 'romfs', 'shader_cache', 'save', 'saves', 'main', 'pkp.exe', 'pkp.ini' -or
    $_.FullName -match '\\updates\\'
}
if ($bad) { $bad | ForEach-Object { Write-Error "game-derived or private file in the package: $($_.FullName)" -ErrorAction Continue }; throw 'package check failed' }

# each launcher file in the package is the one built for it: never the base zip's old launcher, never one in place of the other
function Same([string]$file, [string]$want) { (Test-Path $file) -and (Get-FileHash $file).Hash -eq (Get-FileHash $want).Hash }
function Zip([string]$zipName) {
    $zip = Join-Path $Out $zipName
    if (Test-Path $zip) { Remove-Item -Force $zip }
    [IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    Write-Host "wrote $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MiB)"
}

if (-not (Same (Join-Path $pkg 'Recompia.exe') $Launcher)) { throw 'Recompia.exe in the package is not the launcher just built' }
if (-not (Same (Join-Path $pkg 'PokopiaLauncher.exe') $Stub)) { throw 'PokopiaLauncher.exe in the package is not the starter just built' }
Zip "$name-windows-x64.zip"

# the old-name copy: the same tree under the old folder, the full launcher under the old name, no starter
Rename-Item $pkg $oldName
$pkg = Join-Path $stage $oldName
Remove-Item -Force (Join-Path $pkg 'Recompia.exe')
Put $Launcher 'PokopiaLauncher.exe'
Readme 'PokopiaLauncher.exe'
if (-not (Same (Join-Path $pkg 'PokopiaLauncher.exe') $Launcher)) { throw 'PokopiaLauncher.exe in the old-name copy is not the launcher just built' }
Zip "$oldName-win64.zip"
