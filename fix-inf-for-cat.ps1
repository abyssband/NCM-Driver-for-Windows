#!/usr/bin/env pwsh
# Strip non-WHQL-compliant matches from the BUILT INF (in x64\Release).
# Run after build, before inf2cat.

$ErrorActionPreference = 'Stop'
$inf = Join-Path $PSScriptRoot 'x64\Release\UsbNcmSample.inf'
$raw = Get-Content $inf -Raw -Encoding Unicode

# Remove generic CDC-NCM class match (fails inf2cat WHQL test B2.6.4.9).
$badLine = '%UsbNcm.DeviceDesc%=UsbNcm_Device,USB\Class_02&SubClass_0d&Prot_00'
if ($raw.Contains($badLine)) {
    $raw = $raw.Replace($badLine + "`r`n", '')
    $raw = $raw.Replace($badLine, '')
    Write-Host "Removed: generic CDC-NCM class match"
}

# Also remove its comment line.
$comment = '; Generic CDC-NCM (any compliant NCM device).'
if ($raw.Contains($comment)) {
    $raw = $raw.Replace($comment + "`r`n", '')
}

[System.IO.File]::WriteAllText($inf, $raw, [System.Text.UnicodeEncoding]::new($false, $true))

Write-Host ""
Write-Host "=== Remaining device matches ==="
Get-Content $inf -Encoding Unicode |
    Select-String -Pattern 'UsbNcm_Device|VID_05AC|MS_COMP' |
    ForEach-Object { "  " + $_.Line.Trim() }
