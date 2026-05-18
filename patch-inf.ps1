#!/usr/bin/env pwsh
# Patch host\UsbNcmSample.inf to explicitly match Apple's macOS USB-C NCM device.

$ErrorActionPreference = 'Stop'
$inf = Join-Path $PSScriptRoot 'host\UsbNcmSample.inf'
$raw = Get-Content $inf -Raw -Encoding Unicode

# ─── Patch 1: Replace the single MS_COMP_WINNCM device match line with our
#             expanded set: Apple-specific HWIDs + the original + generic CDC-NCM.

$oldLine = '%UsbNcm.DeviceDesc%=UsbNcm_Device,USB\MS_COMP_WINNCM'

$newBlock = @'
; Apple Quirk: explicitly match macOS USB-C NCM peripherals on both NCM
; communication interfaces (MI_00 and MI_02). A more specific HWID outranks
; the generic class match so this driver wins driver-matching over the
; Microsoft inbox UsbNcm.sys when both are present.
%UsbNcm.DeviceDesc.Apple%=UsbNcm_Device,USB\VID_05AC&PID_1905&MI_00
%UsbNcm.DeviceDesc.Apple%=UsbNcm_Device,USB\VID_05AC&PID_1905&MI_02
; Microsoft-specific NCM extension compatible ID.
%UsbNcm.DeviceDesc%=UsbNcm_Device,USB\MS_COMP_WINNCM
; Generic CDC-NCM (any compliant NCM device — ref NCM10 spec 4.2).
%UsbNcm.DeviceDesc%=UsbNcm_Device,USB\Class_02&SubClass_0d&Prot_00
'@.Replace("`r`n","`n").Replace("`n","`r`n")  # normalize to CRLF

if (-not $raw.Contains($oldLine)) {
    Write-Error "Patch 1 anchor not found"; exit 1
}
$raw = $raw.Replace($oldLine, $newBlock)

# ─── Patch 2: Remove the commented-out generic CDC-NCM line that's now active
#             above (avoid duplicate entries).

$commentedLine = '; %UsbNcm.DeviceDesc%=UsbNcm_Device, USB\Class_02&SubClass_0d&Prot_00'
if ($raw.Contains($commentedLine)) {
    $raw = $raw.Replace($commentedLine + "`r`n", "")
    $raw = $raw.Replace($commentedLine, "")
}

# ─── Patch 3: Remove the leading commentary that's now redundant.

$staleComment = '; Uncomment to install for any NCM-compatible device:'
if ($raw.Contains($staleComment)) {
    $raw = $raw.Replace($staleComment + "`r`n", "")
    $raw = $raw.Replace($staleComment, "")
}
$staleComment2 = '; Prot_00 => No encapsulated commands / responses (ref NCM10 4.2)'
if ($raw.Contains($staleComment2)) {
    $raw = $raw.Replace($staleComment2 + "`r`n", "")
    $raw = $raw.Replace($staleComment2, "")
}

# ─── Patch 4: Add Apple-specific device description string.

$oldDesc = 'UsbNcm.DeviceDesc = "UsbNcm Host Device"'
$newDesc = $oldDesc + "`r`n" + 'UsbNcm.DeviceDesc.Apple = "Apple NCM Host Device (macOS USB-C)"'
if ($raw.Contains($oldDesc)) {
    $raw = $raw.Replace($oldDesc, $newDesc)
} else {
    Write-Warning "Description string not found in expected format — string section unmodified"
}

# ─── Write back as UTF-16LE (project convention).
[System.IO.File]::WriteAllText($inf, $raw, [System.Text.UnicodeEncoding]::new($false, $true))

Write-Host "OK INF patched."
Write-Host ""
Write-Host "=== Device match section after patching ==="
Get-Content $inf -Encoding Unicode |
    Select-String -Pattern 'VID_05AC|MS_COMP_WINNCM|Class_02|DeviceDesc\.Apple' |
    ForEach-Object { "  " + $_.Line.Trim() }
