# Shared by the tools build scripts (dot-source it): finds the MSVC x64 environment script and can import it.
#   . (Join-Path $PSScriptRoot '..\..\scripts\msvc.ps1')
#   $vcvars = Get-VcVars64          path of vcvars64.bat of the newest Visual Studio / Build Tools with the x64 C++ tools
#   Import-MsvcEnv $vcvars          copy the variables vcvars64.bat sets into this session, put LLVM, CMake and Ninja on PATH
function Get-VcVars64 {
    Join-Path (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) 'VC\Auxiliary\Build\vcvars64.bat'
}

function Import-MsvcEnv([string]$vcvars) {
    foreach ($line in (cmd /c "`"$vcvars`" >nul 2>&1 && set")) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
    }
    $env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:PATH"
}
