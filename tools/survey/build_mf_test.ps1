# Builds mf_decode_test.exe (Media Foundation decode test) with clang-cl into work\survey\fmv\.
# Run:  pwsh -File tools\survey\build_mf_test.ps1
$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$out   = [System.IO.Path]::GetFullPath((Join-Path $here '..\..\work\survey\fmv'))
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
foreach ($p in @($clang, $vcvars)) { if (-not (Test-Path $p)) { throw "not found: $p" } }
$envdump = cmd /c "`"$vcvars`" >nul && set"
foreach ($line in $envdump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
New-Item -ItemType Directory -Force $out | Out-Null
& $clang /nologo /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE (Join-Path $here 'mf_decode_test.cpp') "/Fe$out\mf_decode_test.exe" "/Fo$out\\" 2>&1 | ForEach-Object { $_ }
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
Write-Output "built: $out\mf_decode_test.exe"
