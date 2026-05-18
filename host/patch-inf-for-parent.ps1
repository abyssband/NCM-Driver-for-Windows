#!/usr/bin/env pwsh
# Replace the per-MI Apple matches with a single parent VID/PID match.
# This makes our driver replace usbccgp for the device, giving us
# direct access to all 4 interfaces.

$ErrorActionPreference = 'Stop'
$inf = Join-Path $PSScriptRoot 'UsbNcmSample.inf'
$raw = Get-Content $inf -Raw -Encoding Unicode

# Replace the two &MI_xx matches with a single parent VID/PID match.
$old1 = '%UsbNcm.DeviceDesc.Apple%=UsbNcm_Device,USB\VID_05AC&PID_1905&MI_00'
$old2 = '%UsbNcm.DeviceDesc.Apple%=UsbNcm_Device,USB\VID_05AC&PID_1905&MI_02'
$new  = '%UsbNcm.DeviceDesc.Apple%=UsbNcm_Device,USB\VID_05AC&PID_1905'

if ($raw.Contains($old1)) { $raw = $raw.Replace($old1, $new) }
if ($raw.Contains($old2)) {
    # Remove the second line (now redundant)
    $raw = $raw.Replace($old2 + "`r`n", '').Replace($old2, '')
}

[System.IO.File]::WriteAllText($inf, $raw, [System.Text.UnicodeEncoding]::new($false, $true))

Write-Host "INF patched. Device matches now:"
Get-Content $inf -Encoding Unicode |
    Select-String 'UsbNcm_Device|VID_05AC|MS_COMP_WINNCM' |
    ForEach-Object { '  ' + $_.Line.Trim() }
