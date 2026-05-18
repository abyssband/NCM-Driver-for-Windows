param([string]$Vid = "05AC", [string]$Pidx = "1905")

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class UsbIoctl {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, uint nInBufferSize,
        IntPtr lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    public const uint GENERIC_WRITE = 0x40000000;
    public const uint OPEN_EXISTING = 3;
    public const uint FILE_SHARE_READ = 1;
    public const uint FILE_SHARE_WRITE = 2;
    public const uint IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX    = 0x220448;
    public const uint IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2 = 0x220460;

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr SetupDiGetClassDevsW(
        ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr devInfoSet, IntPtr devInfoData, ref Guid interfaceClassGuid,
        uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceInterfaceDetailW(
        IntPtr devInfoSet, ref SP_DEVICE_INTERFACE_DATA interfaceData,
        IntPtr detailData, uint detailSize, ref uint requiredSize, IntPtr deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr devInfoSet);

    public const uint DIGCF_PRESENT = 2;
    public const uint DIGCF_DEVICEINTERFACE = 0x10;

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA {
        public uint cbSize;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public IntPtr Reserved;
    }
}
"@

# GUID_DEVINTERFACE_USB_HUB = {f18a0e88-c30c-11d0-8815-00a0c906bed8}
$hubGuid = [Guid]'f18a0e88-c30c-11d0-8815-00a0c906bed8'

$speedMap = @{
    0='LowSpeed   (1.5Mbps)'
    1='FullSpeed  (12Mbps)'
    2='HighSpeed  (480Mbps  - USB 2.0)'
    3='SuperSpeed (5Gbps    - USB 3.0/3.1g1)'
    4='SuperSpeedPlus (10Gbps - USB 3.1g2)'
}

$hSet = [UsbIoctl]::SetupDiGetClassDevsW([ref]$hubGuid, [IntPtr]::Zero, [IntPtr]::Zero,
    [UsbIoctl]::DIGCF_PRESENT -bor [UsbIoctl]::DIGCF_DEVICEINTERFACE)
if ($hSet -eq [IntPtr]::Zero -or $hSet -eq -1) { "SetupDiGetClassDevs failed"; exit 1 }

$idx = 0
$found = $false
while ($true) {
    $ifd = New-Object UsbIoctl+SP_DEVICE_INTERFACE_DATA
    $ifd.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ifd)
    $ok = [UsbIoctl]::SetupDiEnumDeviceInterfaces($hSet, [IntPtr]::Zero, [ref]$hubGuid, $idx, [ref]$ifd)
    if (-not $ok) { break }

    # Get path
    $req = 0
    [UsbIoctl]::SetupDiGetDeviceInterfaceDetailW($hSet, [ref]$ifd, [IntPtr]::Zero, 0, [ref]$req, [IntPtr]::Zero) | Out-Null
    if ($req -gt 0) {
        $detail = [Runtime.InteropServices.Marshal]::AllocHGlobal([int]$req)
        # cbSize for SP_DEVICE_INTERFACE_DETAIL_DATA_W = 8 on 64-bit, 6 on 32-bit
        [Runtime.InteropServices.Marshal]::WriteInt32($detail, 0, 8)
        try {
            $ok2 = [UsbIoctl]::SetupDiGetDeviceInterfaceDetailW($hSet, [ref]$ifd, $detail, $req, [ref]$req, [IntPtr]::Zero)
            if ($ok2) {
                $path = [Runtime.InteropServices.Marshal]::PtrToStringUni([IntPtr]::Add($detail, 4))
                # Open hub
                $h = [UsbIoctl]::CreateFileW($path, [UsbIoctl]::GENERIC_WRITE,
                    [UsbIoctl]::FILE_SHARE_READ -bor [UsbIoctl]::FILE_SHARE_WRITE,
                    [IntPtr]::Zero, [UsbIoctl]::OPEN_EXISTING, 0, [IntPtr]::Zero)
                if (-not $h.IsInvalid) {
                    # Probe ports 1..32
                    for ($port = 1; $port -le 32; $port++) {
                        $size = 35
                        $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal($size)
                        try {
                            for ($i = 0; $i -lt $size; $i++) { [Runtime.InteropServices.Marshal]::WriteByte($buf, $i, 0) }
                            [Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, $port)
                            $br = 0
                            $ok3 = [UsbIoctl]::DeviceIoControl($h, [UsbIoctl]::IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                                $buf, $size, $buf, $size, [ref]$br, [IntPtr]::Zero)
                            if ($ok3 -and $br -ge 28) {
                                $vendor  = [Runtime.InteropServices.Marshal]::ReadInt16($buf, 12) -band 0xFFFF
                                $product = [Runtime.InteropServices.Marshal]::ReadInt16($buf, 14) -band 0xFFFF
                                $bcdUSB  = [Runtime.InteropServices.Marshal]::ReadInt16($buf, 6)  -band 0xFFFF
                                $speed   = [Runtime.InteropServices.Marshal]::ReadByte($buf, 27)
                                if ($vendor -ne 0 -or $product -ne 0) {
                                    $vidHex = '{0:X4}' -f $vendor
                                    $pidHex = '{0:X4}' -f $product
                                    if ($vidHex -eq $Vid -and $pidHex -eq $Pidx) {
                                        ""
                                        "=== FOUND TARGET DEVICE ==="
                                        "Hub path:   $path"
                                        "Port index: $port"
                                        "VID/PID:    $vidHex / $pidHex"
                                        "bcdUSB:     $('{0:X4}' -f $bcdUSB)"
                                        "Speed:      $($speedMap[[int]$speed]) [enum=$speed]"
                                        $found = $true
                                    }
                                }
                            }
                        } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
                    }
                    $h.Dispose()
                }
            }
        } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($detail) }
    }
    $idx++
}
[UsbIoctl]::SetupDiDestroyDeviceInfoList($hSet) | Out-Null

if (-not $found) { "Device VID=$Vid PID=$Pidx not found on any USB hub" }
