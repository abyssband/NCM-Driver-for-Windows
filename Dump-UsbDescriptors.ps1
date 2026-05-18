# Dump full USB configuration descriptor of the Apple device via hub IOCTL.
# Shows every interface, every endpoint, every SS Companion descriptor.

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public class Usb {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern SafeFileHandle CreateFileW(string p, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool DeviceIoControl(SafeFileHandle h, uint c, IntPtr ib, uint ibs, IntPtr ob, uint obs, out uint br, IntPtr o);
}
"@ -ErrorAction SilentlyContinue

# USB constants
$IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION = 0x220410
$USB_CONFIGURATION_DESCRIPTOR_TYPE = 0x02
$USB_INTERFACE_DESCRIPTOR_TYPE     = 0x04
$USB_ENDPOINT_DESCRIPTOR_TYPE      = 0x05
$USB_SS_ENDPOINT_COMPANION_TYPE    = 0x30
$CS_INTERFACE                       = 0x24

$hubPath = '\\?\usb#root_hub30#7&2b450711&0&0#{f18a0e88-c30c-11d0-8815-00a0c906bed8}'
$port    = 6

# Build the request: USB_DESCRIPTOR_REQUEST struct + buffer
# struct: ULONG ConnectionIndex; struct SetupPacket { UCHAR bmReq; UCHAR bReq; USHORT wValue; USHORT wIndex; USHORT wLength; }; UCHAR Data[];
# bmReq=0x80, bReq=GET_DESCRIPTOR (6), wValue = (CONFIG<<8)|0, wIndex=0, wLength=<bufsize>
$bufSize = 4096
$reqSize = 4 + 8 + $bufSize  # ConnectionIndex + SetupPacket(8) + Data
$req = [Runtime.InteropServices.Marshal]::AllocHGlobal($reqSize)
try {
    for ($i = 0; $i -lt $reqSize; $i++) { [Runtime.InteropServices.Marshal]::WriteByte($req, $i, 0) }
    [Runtime.InteropServices.Marshal]::WriteInt32($req, 0, $port)
    [Runtime.InteropServices.Marshal]::WriteByte($req, 4, 0x80)          # bmRequestType
    [Runtime.InteropServices.Marshal]::WriteByte($req, 5, 0x06)          # bRequest = GET_DESCRIPTOR
    [Runtime.InteropServices.Marshal]::WriteByte($req, 6, 0x00)          # wValue lo
    [Runtime.InteropServices.Marshal]::WriteByte($req, 7, $USB_CONFIGURATION_DESCRIPTOR_TYPE)
    [Runtime.InteropServices.Marshal]::WriteInt16($req, 8, 0)             # wIndex
    [Runtime.InteropServices.Marshal]::WriteInt16($req, 10, $bufSize)     # wLength

    $h = [Usb]::CreateFileW($hubPath, 0x40000000, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h.IsInvalid) { "Cannot open hub"; exit 1 }

    $br = 0
    $ok = [Usb]::DeviceIoControl($h, $IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
        $req, $reqSize, $req, $reqSize, [ref]$br, [IntPtr]::Zero)
    if (-not $ok) { "IOCTL failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }
    $h.Dispose()

    "Got $br bytes from hub"

    # Skip the USB_DESCRIPTOR_REQUEST header (4+8=12 bytes), the rest is config descriptor + everything that follows
    $dataOffset = 12
    $dataLen    = [Runtime.InteropServices.Marshal]::ReadInt16($req, $dataOffset + 2)  # wTotalLength from config descriptor
    "Config descriptor wTotalLength = $dataLen"
    ""

    # Now walk descriptors
    $off = $dataOffset
    $end = $dataOffset + $dataLen
    $currIface = -1
    $currAlt   = -1
    $ifaceEpsLeft = 0

    function Format-EpType {
        param([byte]$attr)
        switch ($attr -band 0x03) {
            0 { "CONTROL" }
            1 { "ISOCHRONOUS" }
            2 { "BULK" }
            3 { "INTERRUPT" }
        }
    }

    while ($off + 2 -le $end) {
        $bLength = [Runtime.InteropServices.Marshal]::ReadByte($req, $off)
        $bType   = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 1)
        if ($bLength -eq 0) { break }

        switch ($bType) {
            $USB_CONFIGURATION_DESCRIPTOR_TYPE {
                $numIfaces = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 4)
                $cfgVal    = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 5)
                $attr      = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 7)
                $maxPwr    = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 8)
                "[CONFIG]    NumInterfaces=$numIfaces  ConfigValue=$cfgVal  Attr=0x$('{0:X2}' -f $attr)  MaxPower=$($maxPwr*2)mA"
            }
            0x0B { # IAD
                $first   = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 2)
                $count   = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 3)
                $cls     = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 4)
                $sub     = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 5)
                $proto   = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 6)
                "  [IAD]    First=$first Count=$count Class=0x$('{0:X2}' -f $cls)/sub=0x$('{0:X2}' -f $sub)/proto=0x$('{0:X2}' -f $proto)"
            }
            $USB_INTERFACE_DESCRIPTOR_TYPE {
                $currIface = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 2)
                $currAlt   = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 3)
                $numEps    = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 4)
                $cls       = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 5)
                $sub       = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 6)
                $proto     = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 7)
                $ifaceEpsLeft = $numEps
                "  [IFACE]  Number=$currIface Alt=$currAlt NumEPs=$numEps Class=0x$('{0:X2}' -f $cls)/sub=0x$('{0:X2}' -f $sub)/proto=0x$('{0:X2}' -f $proto)"
            }
            $USB_ENDPOINT_DESCRIPTOR_TYPE {
                $epAddr  = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 2)
                $attr    = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 3)
                $maxPkt  = [Runtime.InteropServices.Marshal]::ReadInt16($req, $off + 4) -band 0xFFFF
                $interval = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 6)
                $dir = if ($epAddr -band 0x80) { 'IN ' } else { 'OUT' }
                $epNum = $epAddr -band 0x0F
                $epType = Format-EpType $attr
                # SS bulk MaxPacketSize is 1024 (USB 2.0 HS bulk is 512)
                $speedHint = if ($epType -eq 'BULK') {
                    if ($maxPkt -eq 1024) { '  *** SuperSpeed-class bulk (1024B) ***' }
                    elseif ($maxPkt -eq 512) { '  (HighSpeed bulk, 512B)' }
                    else { "  (other size)" }
                } else { '' }
                "    [EP]   Addr=0x$('{0:X2}' -f $epAddr) ($dir EP#$epNum)  Type=$epType  MaxPkt=$maxPkt  Interval=$interval$speedHint"
            }
            $USB_SS_ENDPOINT_COMPANION_TYPE {
                $maxBurst   = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 2)
                $attributes = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 3)
                $bytesPerInt= [Runtime.InteropServices.Marshal]::ReadInt16($req, $off + 4) -band 0xFFFF
                "    [SS-COMP] MaxBurst=$maxBurst  Attr=0x$('{0:X2}' -f $attributes)  BytesPerInterval=$bytesPerInt"
            }
            $CS_INTERFACE {
                $subType = [Runtime.InteropServices.Marshal]::ReadByte($req, $off + 2)
                "    [CS-IF] Subtype=0x$('{0:X2}' -f $subType) Length=$bLength"
            }
            default {
                "    [other] Type=0x$('{0:X2}' -f $bType) Length=$bLength"
            }
        }
        $off += $bLength
    }
} finally {
    [Runtime.InteropServices.Marshal]::FreeHGlobal($req)
}
