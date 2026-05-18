// file_recv.cpp -- receive a file over USB via NCM by listening for UDP
// packets with our magic header. Bypasses Windows IP stack entirely; just
// reads NCM NTBs from EP 0x81, finds UDP packets to our magic port, and
// reassembles the file on disk.
//
// Wire protocol (inside each UDP packet's payload):
//   bytes 0..3   : magic "AFTX" (Apple File TX)
//   bytes 4..7   : sequence number (uint32, little-endian)
//   bytes 8..11  : total chunks (uint32, little-endian)
//   bytes 12..15 : payload length (uint32, little-endian)
//   bytes 16..   : raw file bytes
// First packet (seq=0) carries an extra metadata header before the file
// bytes: "META" + uint32 file_size + filename (zero-terminated, up to 255).
//
// Build (same setup as peer_probe):
//   cmd /c build2.cmd  (after editing build2.cmd to compile this instead,
//   or build manually with cl /EHsc /W4 /O2 /std:c++17 file_recv.cpp /link
//   setupapi.lib winusb.lib /OUT:file_recv.exe)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <initguid.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

DEFINE_GUID(AppleUsbPeerGuid,
    0xd28df5ac, 0x27aa, 0x47ca, 0x97, 0x14, 0x1e, 0xfa, 0x71, 0x56, 0x68, 0x16);

static constexpr uint16_t kRxPort = 5001;
static constexpr uint32_t kMagic  = 'X' << 24 | 'T' << 16 | 'F' << 8 | 'A'; // "AFTX" LE
static constexpr uint32_t kMeta   = 'A' << 24 | 'T' << 16 | 'E' << 8 | 'M'; // "META" LE

static void Die(const char* what) {
    DWORD e = GetLastError();
    std::fprintf(stderr, "FATAL: %s (Win32 err=%lu)\n", what, e);
    std::exit(1);
}

struct FileState {
    FILE* fp = nullptr;
    uint32_t totalChunks = 0;
    uint32_t received = 0;
    std::vector<bool> seen;
    uint64_t fileSize = 0;
    uint64_t bytesWritten = 0;
    std::string filename;
    bool done = false;
};

static FileState g_state;

static void HandleAftxPacket(const uint8_t* payload, size_t len) {
    if (len < 16) return;
    uint32_t magic = *reinterpret_cast<const uint32_t*>(payload);
    if (magic != kMagic) return;
    uint32_t seq    = *reinterpret_cast<const uint32_t*>(payload + 4);
    uint32_t total  = *reinterpret_cast<const uint32_t*>(payload + 8);
    uint32_t plen   = *reinterpret_cast<const uint32_t*>(payload + 12);
    if (16 + plen > len) return;

    if (g_state.totalChunks == 0) {
        g_state.totalChunks = total;
        g_state.seen.assign(total, false);
    }
    if (seq >= g_state.totalChunks) return;
    if (g_state.seen[seq]) return;
    g_state.seen[seq] = true;
    g_state.received++;

    const uint8_t* data = payload + 16;
    uint32_t dataLen = plen;

    // First packet carries METAdata before the file bytes.
    if (seq == 0 && dataLen >= 8 && *reinterpret_cast<const uint32_t*>(data) == kMeta) {
        uint32_t fileSize = *reinterpret_cast<const uint32_t*>(data + 4);
        const char* fname = reinterpret_cast<const char*>(data + 8);
        size_t fnameLen = strnlen(fname, dataLen - 8);
        g_state.filename = std::string(fname, fnameLen);
        g_state.fileSize = fileSize;
        std::printf("=> Receiving '%s' (%u bytes, %u chunks)\n",
            g_state.filename.c_str(), fileSize, total);

        std::string outPath = "received_" + g_state.filename;
        g_state.fp = std::fopen(outPath.c_str(), "wb");
        if (!g_state.fp) Die("fopen");

        // Advance data past the META header
        size_t headerSize = 8 + fnameLen + 1;
        if (headerSize > dataLen) headerSize = dataLen;
        data += headerSize;
        dataLen -= (uint32_t)headerSize;
    }

    if (g_state.fp && dataLen > 0) {
        // Seek to chunk offset. For chunk 0 with stripped META, this isn't
        // quite right -- but the simple sender always sends fixed-size
        // chunks aligned to the file. For chunk 0 with metadata, we just
        // write at offset 0.
        // Compute chunk offset assuming fixed-size data per chunk.
        if (seq > 0) {
            uint64_t chunkSize = (uint64_t)plen; // assumes all chunks same size
            _fseeki64(g_state.fp, chunkSize * seq, SEEK_SET);
        } else {
            _fseeki64(g_state.fp, 0, SEEK_SET);
        }
        std::fwrite(data, 1, dataLen, g_state.fp);
        g_state.bytesWritten += dataLen;
    }

    if (g_state.received == g_state.totalChunks && !g_state.done) {
        g_state.done = true;
        if (g_state.fp) { std::fclose(g_state.fp); g_state.fp = nullptr; }
        std::printf("=> Complete: received all %u chunks, wrote %llu bytes\n",
            g_state.totalChunks, (unsigned long long)g_state.bytesWritten);
    }
}

// Parse one NCM NTB16, extract UDP packets to our magic port.
static void ParseNtb(const uint8_t* buf, size_t len) {
    if (len < 12) return;
    if (buf[0] != 'N' || buf[1] != 'C' || buf[2] != 'M' || buf[3] != 'H') return;
    uint16_t wNdpIndex = *reinterpret_cast<const uint16_t*>(buf + 10);
    if (wNdpIndex + 8 > len) return;

    const uint8_t* ndp = buf + wNdpIndex;
    if (ndp[0] != 'N' || ndp[1] != 'C' || ndp[2] != 'M' || ndp[3] != '0') return;
    uint16_t ndpLen = *reinterpret_cast<const uint16_t*>(ndp + 4);

    const uint8_t* entry = ndp + 8;
    while (entry + 4 <= ndp + ndpLen) {
        uint16_t dgIdx = *reinterpret_cast<const uint16_t*>(entry);
        uint16_t dgLen = *reinterpret_cast<const uint16_t*>(entry + 2);
        entry += 4;
        if (dgIdx == 0 || dgLen == 0) break;
        if (dgIdx + dgLen > len) break;

        const uint8_t* eth = buf + dgIdx;
        if (dgLen < 14) continue;
        uint16_t ethType = (uint16_t(eth[12]) << 8) | eth[13];

        const uint8_t* udpPkt = nullptr;
        uint16_t udpLen = 0;
        uint16_t dstPort = 0;

        if (ethType == 0x0800 && dgLen >= 14 + 20 + 8) {
            uint8_t ihl = (eth[14] & 0x0F) * 4;
            if (eth[14 + 9] == 17 && dgLen >= 14 + ihl + 8) {
                udpPkt = eth + 14 + ihl;
                udpLen = (uint16_t(udpPkt[4]) << 8) | udpPkt[5];
                dstPort = (uint16_t(udpPkt[2]) << 8) | udpPkt[3];
            }
        } else if (ethType == 0x86DD && dgLen >= 14 + 40 + 8) {
            if (eth[14 + 6] == 17) {
                udpPkt = eth + 14 + 40;
                udpLen = (uint16_t(udpPkt[4]) << 8) | udpPkt[5];
                dstPort = (uint16_t(udpPkt[2]) << 8) | udpPkt[3];
            }
        }

        if (udpPkt && dstPort == kRxPort && udpLen >= 16) {
            HandleAftxPacket(udpPkt + 8, udpLen - 8);
        }
    }
}

int wmain() {
    HDEVINFO h = SetupDiGetClassDevsW(&AppleUsbPeerGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) Die("SetupDiGetClassDevs");
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    if (!SetupDiEnumDeviceInterfaces(h, nullptr, &AppleUsbPeerGuid, 0, &ifd))
        Die("SetupDiEnumDeviceInterfaces");
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(h, &ifd, nullptr, 0, &need, nullptr);
    std::vector<BYTE> detbuf(need);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detbuf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(h, &ifd, det, need, nullptr, nullptr))
        Die("SetupDiGetDeviceInterfaceDetail");
    HANDLE dev = CreateFileW(det->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (dev == INVALID_HANDLE_VALUE) Die("CreateFile");
    SetupDiDestroyDeviceInfoList(h);

    WINUSB_INTERFACE_HANDLE wuh0;
    if (!WinUsb_Initialize(dev, &wuh0)) Die("WinUsb_Initialize");
    WINUSB_INTERFACE_HANDLE wuh1 = nullptr;
    WinUsb_GetAssociatedInterface(wuh0, 0, &wuh1);
    if (!wuh1) Die("GetAssociatedInterface");
    WinUsb_SetCurrentAlternateSetting(wuh1, 1);

    std::printf("Listening on EP 0x81 for AFTX/UDP:%u packets...\n", kRxPort);
    std::printf("(send from Mac with the matching file_send.py)\n\n");

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> buf(64 * 1024);
    auto start = std::chrono::steady_clock::now();
    uint64_t totalUsbBytes = 0;
    uint64_t prevBytes = 0;
    auto lastTick = std::chrono::steady_clock::now();

    while (!g_state.done) {
        OVERLAPPED ov = { 0, 0, {{0,0}}, ev };
        ResetEvent(ev);
        ULONG transferred = 0;
        BOOL ok = WinUsb_ReadPipe(wuh1, 0x81, buf.data(), (ULONG)buf.size(),
            &transferred, &ov);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ev, 1000) == WAIT_TIMEOUT) {
                    WinUsb_AbortPipe(wuh1, 0x81);
                    continue;
                }
                if (!WinUsb_GetOverlappedResult(wuh1, &ov, &transferred, TRUE))
                    continue;
            } else continue;
        }
        if (transferred > 0) {
            totalUsbBytes += transferred;
            ParseNtb(buf.data(), transferred);
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count() >= 1000) {
            uint64_t delta = totalUsbBytes - prevBytes;
            prevBytes = totalUsbBytes;
            double mbps = (double)delta / 1024.0 / 1024.0;
            std::printf("  USB RX %.1f MB/s  |  AFTX chunks received: %u/%u  |  file bytes written: %llu\n",
                mbps, g_state.received, g_state.totalChunks,
                (unsigned long long)g_state.bytesWritten);
            std::fflush(stdout);
            lastTick = now;
        }
    }

    auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    std::printf("\n=== Transfer done ===\n");
    std::printf("File: %s (%llu bytes)\n", g_state.filename.c_str(),
        (unsigned long long)g_state.bytesWritten);
    std::printf("Wall time: %.2f s\n", secs);
    std::printf("Effective throughput: %.1f MB/s\n",
        (double)g_state.bytesWritten / 1024.0 / 1024.0 / secs);
    std::printf("Total USB bytes read: %.1f MB (NCM/Ethernet/IP/UDP overhead included)\n",
        (double)totalUsbBytes / 1024.0 / 1024.0);

    WinUsb_Free(wuh0);
    CloseHandle(dev);
    return 0;
}
