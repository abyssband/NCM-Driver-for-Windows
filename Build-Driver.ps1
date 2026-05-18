#!/usr/bin/env pwsh
<#
.SYNOPSIS
    One-stop script to build, optionally sign, and optionally install the
    Apple-Quirk patched UsbNcmSample driver.

.DESCRIPTION
    Walks through the full local build pipeline:
      1. Detect WDK + Visual Studio. Prompt to install if missing.
      2. Build the driver from source.
      3. Optionally generate a self-signed test cert and sign the driver.
      4. Optionally install into the Trusted Root + Trusted Publisher stores.
      5. Optionally install the driver via pnputil.

    Designed to be safe to re-run. Each step is idempotent.

.PARAMETER Configuration
    'Release' (default) or 'Debug'.

.PARAMETER Sign
    Generate a self-signed test cert and sign the built .sys.

.PARAMETER InstallCert
    Install the cert into LocalMachine TrustedRoot + TrustedPublisher stores.
    Requires admin. Implied by -InstallDriver.

.PARAMETER InstallDriver
    Run pnputil to install the driver into the driver store. Requires admin
    AND test-signing mode enabled (or attestation-signed binary).

.PARAMETER EnableTestSigning
    Enable Windows test-signing mode via bcdedit. Requires admin AND reboot.

.EXAMPLE
    # Just build:
    .\Build-Driver.ps1

.EXAMPLE
    # Build + sign + ready to install:
    .\Build-Driver.ps1 -Sign

.EXAMPLE
    # All-in-one (admin shell, after enabling test signing + reboot):
    .\Build-Driver.ps1 -Sign -InstallCert -InstallDriver
#>

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release',

    [switch]$Sign,
    [switch]$InstallCert,
    [switch]$InstallDriver,
    [switch]$EnableTestSigning
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
Set-Location $repoRoot

# Make ANSI colours work on PowerShell 5.1 + Win Terminal.
function Step([string]$msg)    { Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok([string]$msg)      { Write-Host "OK  $msg" -ForegroundColor Green }
function Warn([string]$msg)    { Write-Host "!!  $msg" -ForegroundColor Yellow }
function Fail([string]$msg)    { Write-Host "XX  $msg" -ForegroundColor Red; exit 1 }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = [Security.Principal.WindowsPrincipal]::new($id)
    $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ─── Phase 0: Optionally enable test signing ────────────────────────────────
if ($EnableTestSigning) {
    Step "Enabling Windows test-signing mode"
    if (-not (Test-Admin)) { Fail "Need admin shell to set bcdedit testsigning" }
    bcdedit /set testsigning on
    Ok "testsigning on — REBOOT REQUIRED before driver will load"
    $reboot = Read-Host "Reboot now? [y/N]"
    if ($reboot -eq 'y') { Restart-Computer -Force }
    exit 0
}

# ─── Phase 1: Detect Visual Studio + WDK ────────────────────────────────────
Step "Detecting Visual Studio"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Warn "vswhere.exe not found — Visual Studio not installed"
    Write-Host @"

  Install Visual Studio 2022 (or newer) with the C++ workload:

    winget install Microsoft.VisualStudio.2022.Community

  Then re-run the Visual Studio Installer, click 'Modify' on your VS
  install, and add the following Individual Components:

    [x] Windows Driver Kit
    [x] MSVC v143 - VS 2022 C++ x64/x86 build tools (latest)
    [x] C++ ATL for latest v143 build tools with Spectre Mitigations (x64/x86)
    [x] Windows 11 SDK (10.0.26100.0)

"@
    Fail "Install VS + WDK then re-run this script"
}

$vsInstall = & $vswhere -latest -property installationPath
$vsVersion = & $vswhere -latest -property catalog_productDisplayVersion
Ok "Visual Studio $vsVersion at $vsInstall"

Step "Detecting WDK"
$wdkVer = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -match '^10\.0\.[0-9]+' } |
          Sort-Object Name -Descending |
          Select-Object -First 1
if (-not $wdkVer) {
    Fail "WDK not detected. Install via Visual Studio Installer → Individual Components → 'Windows Driver Kit'"
}
Ok "WDK $($wdkVer.Name) detected"

Step "Detecting MSBuild"
$msbuild = & $vswhere -latest -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { Fail "MSBuild.exe not found" }
Ok "MSBuild at $msbuild"

# ─── Phase 2: Ensure submodules are checked out ─────────────────────────────
Step "Checking git submodules"
$gitOk = $false
try {
    $smStatus = git submodule status 2>&1
    if ($LASTEXITCODE -eq 0) {
        $gitOk = $true
        $uninit = $smStatus | Where-Object { $_ -match '^-' }
        if ($uninit) {
            Warn "Uninitialized submodules found, running git submodule update --init --recursive"
            git submodule update --init --recursive
        }
    }
} catch { }
if (-not $gitOk) { Warn "git not available or not a git repo — assuming submodules are in place" }

# ─── Phase 3: Build ─────────────────────────────────────────────────────────
Step "Building UsbNcmSample.sys ($Configuration / x64)"
$buildLog = Join-Path $repoRoot "build-$Configuration.log"
& $msbuild "usbncm.sln" `
    /p:Configuration=$Configuration `
    /p:Platform=x64 `
    /m /nologo /v:minimal `
    /fl /flp:logfile="$buildLog;verbosity=detailed"

if ($LASTEXITCODE -ne 0) {
    Fail "Build failed. See $buildLog"
}
Ok "Build succeeded"

# Locate the output
$sys = Get-ChildItem "x64\$Configuration" -Recurse -Filter UsbNcmSample.sys |
       Select-Object -First 1
if (-not $sys) { Fail "Cannot find built UsbNcmSample.sys" }
Ok "Output: $($sys.FullName) ($([math]::Round($sys.Length/1KB,1)) KB)"

# Locate matching INF
$inf = Get-ChildItem "x64\$Configuration" -Recurse -Filter UsbNcmSample.inf |
       Select-Object -First 1
if (-not $inf) { $inf = Get-Item "host\UsbNcmSample.inf" }
Ok "INF:    $($inf.FullName)"

# ─── Phase 4: Sign ──────────────────────────────────────────────────────────
if ($Sign) {
    Step "Generating self-signed test code-signing cert"
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject "CN=AppleNCM-Dev-$(Get-Date -Format yyyyMMdd)" `
        -KeyAlgorithm RSA -KeyLength 2048 `
        -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(2)
    Ok "Cert subject: $($cert.Subject)"
    Ok "Cert thumbprint: $($cert.Thumbprint)"

    Step "Locating signtool.exe"
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
                Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
                Sort-Object FullName -Descending |
                Select-Object -First 1
    if (-not $signtool) { Fail "signtool.exe not found in WDK" }
    Ok "signtool: $($signtool.FullName)"

    Step "Signing $($sys.Name)"
    & $signtool.FullName sign /v /fd SHA256 /a /s My /n $cert.Subject `
        /tr http://timestamp.digicert.com /td SHA256 `
        $sys.FullName
    if ($LASTEXITCODE -ne 0) { Fail "signtool sign failed" }
    Ok "Signed"

    # Export cert next to the .sys for distribution
    $cerPath = Join-Path $sys.Directory.FullName "AppleNCM-DevCert.cer"
    Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null
    Ok "Cert exported: $cerPath"
    $script:DevCert = $cert
    $script:CerPath = $cerPath
}

# ─── Phase 5: Install cert ──────────────────────────────────────────────────
if ($InstallCert -or $InstallDriver) {
    if (-not (Test-Admin)) { Fail "Need admin shell for -InstallCert / -InstallDriver" }
    if (-not $script:CerPath) { Fail "No cert to install — re-run with -Sign first" }

    Step "Installing cert into TrustedRoot + TrustedPublisher (LocalMachine)"
    Import-Certificate -FilePath $script:CerPath `
        -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $script:CerPath `
        -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Ok "Cert installed into LocalMachine\Root and LocalMachine\TrustedPublisher"
}

# ─── Phase 6: Install driver ───────────────────────────────────────────────
if ($InstallDriver) {
    if (-not (Test-Admin)) { Fail "Need admin shell" }

    Step "Checking test-signing mode"
    $bcd = bcdedit /enum '{current}' 2>&1 | Out-String
    if ($bcd -notmatch 'testsigning\s+Yes') {
        Warn "test-signing is OFF. Driver will not load until you:"
        Write-Host "    bcdedit /set testsigning on" -ForegroundColor Yellow
        Write-Host "    shutdown /r /t 0" -ForegroundColor Yellow
        Write-Host "  Or re-run this script with -EnableTestSigning"
    }

    Step "Installing driver via pnputil"
    & pnputil /add-driver $inf.FullName /install
    if ($LASTEXITCODE -ne 0) { Fail "pnputil failed" }
    Ok "Driver installed. Plug in the Mac via USB-C now."

    Write-Host ""
    Step "Verify with:"
    Write-Host "    Get-PnpDevice -InstanceId 'USB\\VID_05AC&PID_1905*' | Format-List Status, Service"
}

Write-Host ""
Ok "Done."
