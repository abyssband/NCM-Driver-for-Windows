# Build peer_probe.exe — pure user-mode WinUSB tool. No WDK, no MSBuild.
# Just locates cl.exe via vswhere and compiles.

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found" }

$vsInstall = & $vswhere -latest -property installationPath
if (-not $vsInstall) { throw "Visual Studio not found" }

$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

Write-Host "Using $vcvars" -ForegroundColor Cyan

# Run vcvars then cl in the same cmd subprocess, capture environment as side effect.
# We just need cl to be on PATH for one invocation.
$cmd = @"
call "$vcvars" >nul 2>&1
cl /nologo /EHsc /W4 /O2 /std:c++17 ^
   peer_probe.cpp ^
   /link setupapi.lib winusb.lib ^
   /OUT:peer_probe.exe
"@

$cmd | cmd /Q

if (-not (Test-Path peer_probe.exe)) {
    throw "Build failed — peer_probe.exe not produced"
}
$exe = Get-Item peer_probe.exe
Write-Host ("OK: peer_probe.exe ({0:N1} KB)" -f ($exe.Length / 1KB)) -ForegroundColor Green
