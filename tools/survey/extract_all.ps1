# Extract every .pak under game\romfs with retrotool into work\survey\paks\<relpath>\ ; per-pak stdout kept in work\survey\logs\
# usage: pwsh tools\survey\extract_all.ps1 [-Throttle 12]
param([int]$Throttle = 12)
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$rt = "$root\toolchain\retrotool\retrotool.exe"
$rom = "$root\game\romfs"
$out = "$root\work\survey\paks"
$logs = "$root\work\survey\logs"
New-Item -ItemType Directory -Force $out, $logs | Out-Null
Get-ChildItem $rom -Recurse -Filter *.pak | ForEach-Object -Parallel {
    $rel = $_.FullName.Substring($using:rom.Length + 1)
    $dst = Join-Path $using:out ($rel -replace '\.pak$','')
    $log = $using:logs + [char]92 + $rel.Replace([string][char]92,'__') + '.log'
    New-Item -ItemType Directory -Force $dst | Out-Null
    & $using:rt pak extract $_.FullName $dst *> $log
    if ($LASTEXITCODE -ne 0) { "FAIL $rel exit=$LASTEXITCODE" }
} -ThrottleLimit $Throttle
"done"
