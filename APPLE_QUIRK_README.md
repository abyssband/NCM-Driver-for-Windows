# Apple NCM Quirk Patch

This fork adds support for macOS USB-C peripherals (Apple Silicon Macs presenting
themselves as USB CDC-NCM devices with VID `0x05AC` PID `0x1905`).

## The bug

When a modern Mac is plugged into a Windows host over USB-C, macOS exposes
itself as a CDC-NCM network function. Windows' inbox `UsbNcm.sys` (built from
this very repository) **refuses to start** on these devices with
`STATUS_DEVICE_HARDWARE_ERROR` (Code 10 in Device Manager).

Root cause: Apple's NCM function omits the interrupt endpoint on the control
interface. The CDC-NCM specification permits 0 or 1 interrupt endpoints, but
the Microsoft driver requires **exactly 1**.

## The fix

Four targeted changes in [`host/device.cpp`](host/device.cpp). All four are
gated behind a "pipe count == 0" check, so any compliant device that ships
with the interrupt endpoint continues to work unchanged. No behavior change
on standard NCM peripherals — only Apple's non-standard layout is newly
accepted.

| Location | Change |
|---|---|
| `InitializeDevice()` line ~190 | Drop `m_ControlInterruptPipe` from the pipes-complete assert |
| `RetrieveInterruptPipe()` line ~620 | Accept 0 pipes as "no link notifications"; return `STATUS_SUCCESS` with `m_ControlInterruptPipe = nullptr` |
| `EnterWorkingState()` line ~748 | Skip `StartPipe()` when interrupt pipe is null |
| `LeaveWorkingState()` line ~760 | Skip `StopPipe()` when interrupt pipe is null |

## Functional impact of missing interrupt endpoint

The interrupt endpoint normally delivers:

* `USB_CDC_NOTIFICATION_NETWORK_CONNECTION` — link up/down
* `USB_CDC_NOTIFICATION_CONNECTION_SPEED_CHANGE` — link speed updates

Without it, the driver:

* Assumes link is up while the device is attached (reasonable: USB present ⇒ cable plugged ⇒ link up)
* Cannot report a connection speed that updates dynamically (cosmetic only)

For practical use cases — file transfer, SMB, SSH — there is **no observable
behavior difference** from a fully compliant NCM device.

## Build

Same as upstream: open `usbncm.sln` in Visual Studio with the Windows Driver
Kit (WDK) installed, build the `UsbNcmSample` (host) target.

```pwsh
# Install Visual Studio Community + WDK:
winget install Microsoft.VisualStudio.2022.Community
winget install Microsoft.WindowsSDK.10.0.26100
# Then add the "Windows Driver Kit" extension from VS Installer

msbuild usbncm.sln /p:Configuration=Release /p:Platform=x64
```

The resulting `UsbNcmSample.sys` lives in
`x64\Release\UsbNcmSample\UsbNcmSample.sys`.

## Sign + install

Kernel drivers must be signed. Two paths:

### A. Test signing (quickest, single machine, dev only)

```pwsh
# Enable test mode and reboot
bcdedit /set testsigning on
shutdown /r /t 0

# After reboot, watermark "Test Mode" will appear bottom-right (normal)

# Generate a self-signed cert
$cert = New-SelfSignedCertificate -Type CodeSigningCert `
    -Subject "CN=AppleNCMTestCert" `
    -KeyAlgorithm RSA -KeyLength 2048 `
    -CertStoreLocation Cert:\CurrentUser\My

# Sign the .sys
$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
& $signtool sign /v /fd SHA256 /a `
    /s My /n "AppleNCMTestCert" `
    "x64\Release\UsbNcmSample\UsbNcmSample.sys"

# Export cert and trust it
Export-Certificate -Cert $cert -FilePath "$env:USERPROFILE\AppleNCMTestCert.cer"
Import-Certificate -FilePath "$env:USERPROFILE\AppleNCMTestCert.cer" `
    -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath "$env:USERPROFILE\AppleNCMTestCert.cer" `
    -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

### B. Microsoft Attestation Signing (free, official, redistributable)

Register a free Microsoft Partner Center account, submit the .sys via the
Hardware Dashboard, get back a signed binary in 1-3 days. Suitable for
shipping the driver to other users without enabling their test mode.

Out of scope for this README — see
https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/attestation-signing-a-kernel-driver-for-public-release

## Install the driver

```pwsh
# Install via pnputil (admin shell)
pnputil /add-driver host\UsbNcmSample.inf /install

# Or via Device Manager:
#   Right-click "USB Composite Device" under your Apple NCM device
#   → Update driver → Browse → Pick this folder
```

## Verify it worked

```pwsh
Get-PnpDevice -InstanceId 'USB\VID_05AC&PID_1905&MI_00*' |
    Format-List Status, Service, FriendlyName
```

Expected:

```
Status         : OK
Service        : UsbNcmSample
FriendlyName   : UsbNcm Host Device  (or similar)
```

And a new network adapter should appear:

```pwsh
Get-NetAdapter | Where-Object { $_.InterfaceDescription -like "*NCM*" }
```

## Test connectivity

On the Mac:
1. System Settings → General → Sharing → Internet Sharing
2. Share from Wi-Fi, To: the new USB interface
3. (Or set static IP on USB interface)

On Windows:
1. The new NCM adapter should DHCP an address (typically `192.168.x.x`)
2. `ping <mac-ip>` should work
3. SMB / SSH / HTTP to the Mac should now work over the USB cable

Throughput expectation: **5+ Gbps over USB4** (per Linux measurements with
the equivalent patch), **~1-2 Gbps over USB 3.x**.

## Upstream

The Linux equivalent of this patch is at
https://github.com/kanalo-shrek/apple-ncm/tree/main/linux
(working as of 2026-05). The Windows folder in that repo is "Coming Soon" —
this fork may be a useful starting point for them.

Original blog post documenting the underlying issue:
https://arewecooked.dev/blog/mac-to-pc-usb-c-networking

## License

Same as upstream Microsoft repository (MIT, see `LICENSE`).
