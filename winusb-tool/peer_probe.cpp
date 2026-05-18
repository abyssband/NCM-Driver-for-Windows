// peer_probe.cpp — user-mode WinUSB tool for the Apple USB-C peer device.
//
// What it does:
//   1. Find the device by our DeviceInterfaceGUID (set by AppleUsbPeer.inf).
//   2. Open a WinUSB handle, dump all 4 interfaces + endpoints (sanity).
//   3. Activate alt-setting 1 on interface 1 (NCM #1 data) and interface 3
//      (NCM #2 data) so their bulk endpoints come live.
//   4. Spawn one reader thread per IN endpoint (EP 0x81 and EP 0x82) and
//      print per-pipe RX byte counters every 1 s.
//
// Goal: verify whether Mac's anpi3 actually sends bytes on EP 0x82 when
// asked to. Compare to EP 0x81 (the standard NCM #1 channel).
//
// Build:
//   cl /EHsc /W4 /O2 /std:c++17 peer_probe.cpp setupapi.lib winusb.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <cfgmgr32.h>
#include <initguid.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

// Must match the GUID in AppleUsbPeer.inf
// {D28DF5AC-27AA-47CA-9714-1EFA71566816}
DEFINE_GUID(AppleUsbPeerGuid,
    0xd28df5ac, 0x27aa, 0x47ca, 0x97, 0x14, 0x1e, 0xfa, 0x71, 0x56, 0x68, 0x16);

// ---- Logging helpers ------------------------------------------------------

static void Die(const char* what) {
    DWORD e = GetLastError();
    std::fprintf(stderr, "FATAL: %s (Win32 err=%lu / 0x%08lx)\n", what, e, e);
    std::exit(1);
}

static const char* PipeTypeStr(USBD_PIPE_TYPE t) {
    switch (t) {
        case UsbdPipeTypeControl:     return "CONTROL";
        case UsbdPipeTypeIsochronous: return "ISOCHRONOUS";
        case UsbdPipeTypeBulk:        return "BULK";
        case UsbdPipeTypeInterrupt:   return "INTERRUPT";
        default: return "?";
    }
}

// ---- Device discovery -----------------------------------------------------

static std::wstring FindDevicePath() {
    HDEVINFO h = SetupDiGetClassDevsW(
        &AppleUsbPeerGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) Die("SetupDiGetClassDevs");

    SP_DEVICE_INTERFACE_DATA ifd = {};
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(h, nullptr, &AppleUsbPeerGuid, 0, &ifd)) {
        SetupDiDestroyDeviceInfoList(h);
        std::fprintf(stderr, "Device not found. Is the WinUSB INF installed and the device plugged in?\n");
        std::exit(2);
    }

    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(h, &ifd, nullptr, 0, &need, nullptr);
    std::vector<BYTE> buf(need);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(h, &ifd, det, need, nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(h);
        Die("SetupDiGetDeviceInterfaceDetail");
    }

    std::wstring path = det->DevicePath;
    SetupDiDestroyDeviceInfoList(h);
    return path;
}

// ---- Reader thread --------------------------------------------------------

struct PipeCtx {
    WINUSB_INTERFACE_HANDLE wuh;
    UCHAR pipeId;
    const char* label;
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> packets{0};
    std::atomic<bool> stop{false};
    std::atomic<DWORD> lastErr{0};
    // Optional NCM parsing
    bool parseNcm = false;
    std::atomic<uint64_t> ncmDatagrams{0};
    std::atomic<uint64_t> ncmEthBytes{0};
    std::atomic<uint64_t> ncmUdpPayload{0};
    std::atomic<uint64_t> ncmIpv6{0};
    std::atomic<uint64_t> ncmIpv4{0};
};

// Minimal CDC-NCM NTB16 parser. Just enough to count Ethernet frames and
// extract UDP payload for end-to-end throughput measurement.
//
// NTB16 layout:
//   NTH16:  signature "NCMH" (4) + wHeaderLength (2) + wSequence (2) +
//           wBlockLength (2) + wNdpIndex (2)  = 12 bytes
//   NDP16: signature "NCM0" (4) + wLength (2) + wNextNdpIndex (2) +
//          followed by N entries of (wDatagramIndex, wDatagramLength) pairs,
//          terminated by (0, 0)
//
// Datagram = raw Ethernet frame: dst(6) + src(6) + type(2) + payload
static void ParseNtb(const BYTE* buf, ULONG len, PipeCtx* ctx) {
    if (len < 12) return;
    if (buf[0] != 'N' || buf[1] != 'C' || buf[2] != 'M' || buf[3] != 'H') return;
    USHORT wNdpIndex = *reinterpret_cast<const USHORT*>(buf + 10);
    if (wNdpIndex + 8 > len) return;

    const BYTE* ndp = buf + wNdpIndex;
    if (ndp[0] != 'N' || ndp[1] != 'C' || ndp[2] != 'M' || ndp[3] != '0') return;
    USHORT ndpLen = *reinterpret_cast<const USHORT*>(ndp + 4);
    if (wNdpIndex + ndpLen > len) return;

    // Datagram entries start at ndp+8, 4 bytes each (index, length).
    const BYTE* entry = ndp + 8;
    while (entry + 4 <= ndp + ndpLen) {
        USHORT dgIdx = *reinterpret_cast<const USHORT*>(entry);
        USHORT dgLen = *reinterpret_cast<const USHORT*>(entry + 2);
        entry += 4;
        if (dgIdx == 0 || dgLen == 0) break;  // terminator
        if (dgIdx + dgLen > len) break;

        const BYTE* eth = buf + dgIdx;
        ctx->ncmDatagrams.fetch_add(1, std::memory_order_relaxed);
        ctx->ncmEthBytes.fetch_add(dgLen, std::memory_order_relaxed);

        if (dgLen < 14) continue;
        USHORT ethType = (USHORT(eth[12]) << 8) | eth[13];

        if (ethType == 0x0800 && dgLen >= 14 + 20) {
            // IPv4 — check if UDP
            ctx->ncmIpv4.fetch_add(1, std::memory_order_relaxed);
            BYTE ihl = (eth[14] & 0x0F) * 4;
            if (dgLen >= 14 + ihl + 8 && eth[14 + 9] == 17 /*UDP*/) {
                USHORT udpLen = (USHORT(eth[14 + ihl + 4]) << 8) | eth[14 + ihl + 5];
                if (udpLen >= 8) {
                    ctx->ncmUdpPayload.fetch_add(udpLen - 8, std::memory_order_relaxed);
                }
            }
        } else if (ethType == 0x86DD && dgLen >= 14 + 40) {
            // IPv6 — check if UDP (next-header field at byte 6 of IPv6 header)
            ctx->ncmIpv6.fetch_add(1, std::memory_order_relaxed);
            if (eth[14 + 6] == 17 /*UDP*/) {
                USHORT udpLen = (USHORT(eth[14 + 40 + 4]) << 8) | eth[14 + 40 + 5];
                if (udpLen >= 8) {
                    ctx->ncmUdpPayload.fetch_add(udpLen - 8, std::memory_order_relaxed);
                }
            }
        }
    }
}
using ReaderCtx = PipeCtx;
using WriterCtx = PipeCtx;

static void WriterThread(WriterCtx* ctx) {
    // Pump 64 KB chunks of random bytes as fast as possible. The Mac side
    // will see this as NCM data, fail to parse (invalid NTB), and probably
    // drop — but USB bulk OUT bytes still cross the wire, which is what we
    // measure.
    std::vector<BYTE> buf(64 * 1024);
    for (size_t i = 0; i < buf.size(); i++) buf[i] = (BYTE)(i & 0xff);

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) { ctx->lastErr = GetLastError(); return; }

    while (!ctx->stop.load(std::memory_order_relaxed)) {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        ULONG transferred = 0;
        BOOL ok = WinUsb_WritePipe(
            ctx->wuh, ctx->pipeId,
            buf.data(), static_cast<ULONG>(buf.size()),
            &transferred, &ov);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                DWORD w = WaitForSingleObject(ev, 2000);
                if (w == WAIT_TIMEOUT) {
                    WinUsb_AbortPipe(ctx->wuh, ctx->pipeId);
                    ctx->lastErr = ERROR_TIMEOUT;
                    continue;
                }
                if (!WinUsb_GetOverlappedResult(ctx->wuh, &ov, &transferred, TRUE)) {
                    ctx->lastErr = GetLastError();
                    Sleep(20);
                    continue;
                }
            } else {
                ctx->lastErr = e;
                Sleep(50);
                continue;
            }
        }
        ctx->bytes.fetch_add(transferred, std::memory_order_relaxed);
        ctx->packets.fetch_add(1, std::memory_order_relaxed);
    }
    CloseHandle(ev);
}

static void ReaderThread(ReaderCtx* ctx) {
    std::vector<BYTE> buf(64 * 1024);  // 64 KB transfer ceiling
    // Use overlapped so we can timeout cleanly. WinUSB inherits the
    // device handle's overlapped flag; we set FILE_FLAG_OVERLAPPED at open.
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) { ctx->lastErr = GetLastError(); return; }

    while (!ctx->stop.load(std::memory_order_relaxed)) {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);

        ULONG transferred = 0;
        BOOL ok = WinUsb_ReadPipe(
            ctx->wuh, ctx->pipeId,
            buf.data(), static_cast<ULONG>(buf.size()),
            &transferred, &ov);

        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                // Wait at most 500 ms so we can poll ctx->stop frequently.
                DWORD w = WaitForSingleObject(ev, 500);
                if (w == WAIT_TIMEOUT) {
                    WinUsb_AbortPipe(ctx->wuh, ctx->pipeId);
                    continue;
                }
                if (!WinUsb_GetOverlappedResult(ctx->wuh, &ov, &transferred, TRUE)) {
                    ctx->lastErr = GetLastError();
                    Sleep(50);
                    continue;
                }
            } else {
                ctx->lastErr = e;
                Sleep(50);
                continue;
            }
        }

        if (transferred > 0) {
            ctx->bytes.fetch_add(transferred, std::memory_order_relaxed);
            ctx->packets.fetch_add(1, std::memory_order_relaxed);
            if (ctx->parseNcm) {
                ParseNtb(buf.data(), transferred, ctx);
            }
        }
    }
    CloseHandle(ev);
}

// ---- Main -----------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    bool dumpOnly = false;
    bool txMode   = false;
    bool tx1Only  = false;
    bool tx2Only  = false;
    bool ncmParse = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--dump") dumpOnly = true;
        else if (a == L"--tx")     txMode = true;
        else if (a == L"--tx1")    { txMode = true; tx1Only = true; }
        else if (a == L"--tx2")    { txMode = true; tx2Only = true; }
        else if (a == L"--ncm")    ncmParse = true;
    }

    std::wstring path = FindDevicePath();
    std::wprintf(L"Device path: %s\n", path.c_str());

    HANDLE dev = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (dev == INVALID_HANDLE_VALUE) Die("CreateFile device");

    WINUSB_INTERFACE_HANDLE wuh0;
    if (!WinUsb_Initialize(dev, &wuh0)) Die("WinUsb_Initialize");
    std::printf("WinUsb_Initialize OK -- interface 0 handle is live.\n");

    // ---- Device descriptor: find bNumConfigurations ----
    USB_DEVICE_DESCRIPTOR dd = {};
    ULONG got = 0;
    if (WinUsb_GetDescriptor(wuh0, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                             (PUCHAR)&dd, sizeof(dd), &got)) {
        std::printf("\nDevice descriptor:\n");
        std::printf("  bcdUSB              = 0x%04X\n", dd.bcdUSB);
        std::printf("  bDeviceClass        = 0x%02X / sub 0x%02X / proto 0x%02X\n",
            dd.bDeviceClass, dd.bDeviceSubClass, dd.bDeviceProtocol);
        std::printf("  bMaxPacketSize0     = %u\n", dd.bMaxPacketSize0);
        std::printf("  VID/PID             = 0x%04X / 0x%04X\n", dd.idVendor, dd.idProduct);
        std::printf("  bcdDevice           = 0x%04X\n", dd.bcdDevice);
        std::printf("  bNumConfigurations  = %u  <<<<<\n", dd.bNumConfigurations);

        // Dump each configuration descriptor's header
        for (UCHAR ci = 0; ci < dd.bNumConfigurations; ci++) {
            UCHAR cfgBuf[9] = {};
            ULONG cgot = 0;
            if (WinUsb_GetDescriptor(wuh0, USB_CONFIGURATION_DESCRIPTOR_TYPE, ci, 0,
                                     cfgBuf, sizeof(cfgBuf), &cgot)) {
                auto* cd = reinterpret_cast<USB_CONFIGURATION_DESCRIPTOR*>(cfgBuf);
                std::printf("  Config %u: wTotalLength=%u  bNumInterfaces=%u  bConfigurationValue=%u  Attr=0x%02X  MaxPower=%umA\n",
                    ci, cd->wTotalLength, cd->bNumInterfaces, cd->bConfigurationValue,
                    cd->bmAttributes, cd->MaxPower * 2);
            } else {
                std::printf("  Config %u: GetDescriptor failed err=%lu\n", ci, GetLastError());
            }
        }
    } else {
        std::printf("WinUsb_GetDescriptor(device) failed err=%lu\n", GetLastError());
    }

    // Get sibling interface handles.
    WINUSB_INTERFACE_HANDLE wuh[4] = { wuh0, nullptr, nullptr, nullptr };
    for (UCHAR i = 0; i < 3; i++) {
        if (!WinUsb_GetAssociatedInterface(wuh0, i, &wuh[i + 1])) {
            std::printf("  no associated interface @ idx %d (err=%lu)\n", i, GetLastError());
        }
    }

    // Dump descriptors per interface.
    for (UCHAR i = 0; i < 4; i++) {
        if (!wuh[i]) continue;
        USB_INTERFACE_DESCRIPTOR id = {};
        // Try alt 0 first
        if (WinUsb_QueryInterfaceSettings(wuh[i], 0, &id)) {
            std::printf("Interface %u  Alt 0  Class=0x%02X Sub=0x%02X Proto=0x%02X NumEPs=%u\n",
                id.bInterfaceNumber, id.bInterfaceClass,
                id.bInterfaceSubClass, id.bInterfaceProtocol, id.bNumEndpoints);
        }
        // Try alt 1 if it exists
        USB_INTERFACE_DESCRIPTOR id1 = {};
        if (WinUsb_QueryInterfaceSettings(wuh[i], 1, &id1)) {
            std::printf("Interface %u  Alt 1  Class=0x%02X Sub=0x%02X Proto=0x%02X NumEPs=%u\n",
                id1.bInterfaceNumber, id1.bInterfaceClass,
                id1.bInterfaceSubClass, id1.bInterfaceProtocol, id1.bNumEndpoints);

            // Activate alt 1 and enumerate pipes.
            if (!WinUsb_SetCurrentAlternateSetting(wuh[i], 1)) {
                std::printf("  SetCurrentAlternateSetting(1) failed err=%lu\n", GetLastError());
            } else {
                for (UCHAR p = 0; p < id1.bNumEndpoints; p++) {
                    WINUSB_PIPE_INFORMATION pi = {};
                    if (WinUsb_QueryPipe(wuh[i], 1, p, &pi)) {
                        std::printf("    pipe %u  EP 0x%02X  Type=%s  MaxPkt=%u  Interval=%u\n",
                            p, pi.PipeId, PipeTypeStr(pi.PipeType),
                            pi.MaximumPacketSize, pi.Interval);
                    }
                }
            }
        }
    }

    if (dumpOnly) {
        WinUsb_Free(wuh0);
        CloseHandle(dev);
        return 0;
    }

    std::printf("\n");
    if (txMode) {
        std::printf("=== TX mode: pumping 64KB chunks ===\n");
        if (!tx2Only) std::printf("  EP 0x01 (NCM #1 OUT) writer on interface 1\n");
        if (!tx1Only) std::printf("  EP 0x02 (NCM #2 OUT) writer on interface 3\n");
    } else {
        std::printf("=== RX mode: reading from both NCM IN endpoints ===\n");
        std::printf("  EP 0x81 (NCM #1 IN, en6 path) on interface 1\n");
        std::printf("  EP 0x82 (NCM #2 IN, anpi3 path) on interface 3\n");
    }
    std::printf("Press Ctrl+C to stop.\n\n");

    // RX contexts
    ReaderCtx ctx81 = { wuh[1], 0x81, "EP 0x81" };
    ReaderCtx ctx82 = { wuh[3], 0x82, "EP 0x82" };
    ctx81.parseNcm = ncmParse;
    ctx82.parseNcm = ncmParse;
    // TX contexts
    WriterCtx ctx01 = { wuh[1], 0x01, "EP 0x01" };
    WriterCtx ctx02 = { wuh[3], 0x02, "EP 0x02" };

    std::vector<std::thread> threads;
    if (!txMode) {
        threads.emplace_back(ReaderThread, &ctx81);
        threads.emplace_back(ReaderThread, &ctx82);
    } else {
        if (!tx2Only) threads.emplace_back(WriterThread, &ctx01);
        if (!tx1Only) threads.emplace_back(WriterThread, &ctx02);
    }

    auto start = std::chrono::steady_clock::now();
    uint64_t prev81 = 0, prev82 = 0, prev01 = 0, prev02 = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto t = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (!txMode) {
            uint64_t b81 = ctx81.bytes.load(), b82 = ctx82.bytes.load();
            uint64_t d81 = b81 - prev81, d82 = b82 - prev82;
            prev81 = b81; prev82 = b82;
            double mbps81 = (double)d81 / (1024.0 * 1024.0);
            double mbps82 = (double)d82 / (1024.0 * 1024.0);
            std::printf("[t=%llds] RX  EP81 %.1f MB/s (%llu pkts)  |  EP82 %.1f MB/s (%llu pkts)",
                static_cast<long long>(t), mbps81,
                (unsigned long long)ctx81.packets.load(), mbps82,
                (unsigned long long)ctx82.packets.load());
            if (ncmParse) {
                std::printf("  ||  NCM: dgrams=%llu eth=%llu IPv4=%llu IPv6=%llu UDP_payload=%llu",
                    (unsigned long long)(ctx81.ncmDatagrams.load() + ctx82.ncmDatagrams.load()),
                    (unsigned long long)(ctx81.ncmEthBytes.load() + ctx82.ncmEthBytes.load()),
                    (unsigned long long)(ctx81.ncmIpv4.load() + ctx82.ncmIpv4.load()),
                    (unsigned long long)(ctx81.ncmIpv6.load() + ctx82.ncmIpv6.load()),
                    (unsigned long long)(ctx81.ncmUdpPayload.load() + ctx82.ncmUdpPayload.load()));
            }
            std::printf("\n");
        } else {
            uint64_t b01 = ctx01.bytes.load(), b02 = ctx02.bytes.load();
            uint64_t d01 = b01 - prev01, d02 = b02 - prev02;
            prev01 = b01; prev02 = b02;
            double mbps01 = (double)d01 / (1024.0 * 1024.0);
            double mbps02 = (double)d02 / (1024.0 * 1024.0);
            double total  = mbps01 + mbps02;
            std::printf("[t=%llds] TX  EP01  %.1f MB/s (err=%lu)"
                        "  |  EP02  %.1f MB/s (err=%lu)"
                        "  |  TOTAL %.1f MB/s\n",
                static_cast<long long>(t),
                mbps01, ctx01.lastErr.load(),
                mbps02, ctx02.lastErr.load(),
                total);
        }
        std::fflush(stdout);
    }

    ctx81.stop = true; ctx82.stop = true;
    ctx01.stop = true; ctx02.stop = true;
    for (auto& th : threads) th.join();
    WinUsb_Free(wuh0);
    CloseHandle(dev);
    return 0;
}
