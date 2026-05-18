#!/usr/bin/env python3
"""file_send.py -- send a file from Mac to Windows over USB-C.

Runs on Mac. Uses UDP broadcast on bridge100 (no Windows-side IP needed).
Frames each chunk with our AFTX magic so the Windows-side file_recv.exe
can pick our packets out of the NCM stream.

Usage:  python3 file_send.py <path> [target_mbps]
"""

import os
import socket
import struct
import sys
import time

DST_IP    = "192.168.2.255"   # broadcast on bridge100
DST_PORT  = 5001
MAGIC     = b"AFTX"
META_TAG  = b"META"
CHUNK     = 1400               # bytes of file content per packet
HEADER    = 16                 # AFTX + seq + total + plen

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    target_mbps = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

    size = os.path.getsize(path)
    name = os.path.basename(path)
    # First chunk has metadata before the file body. Reserve room for that.
    meta = META_TAG + struct.pack("<I", size) + name.encode("utf-8") + b"\x00"
    first_data_cap = CHUNK - len(meta)
    if first_data_cap < 0:
        print(f"filename too long ({len(name)} bytes); shorten it")
        sys.exit(1)

    # Total chunks: first chunk fits (CHUNK - meta_size) bytes of file; rest fit CHUNK each.
    remaining = size - first_data_cap
    if remaining <= 0:
        total = 1
    else:
        total = 1 + (remaining + CHUNK - 1) // CHUNK

    print(f"sending '{name}' ({size:,} bytes) -> {DST_IP}:{DST_PORT} in {total} chunks, target {target_mbps:.1f} MB/s")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16 * 1024 * 1024)

    target_bps = target_mbps * 1024 * 1024
    bytes_per_pkt = CHUNK + 42  # rough wire overhead estimate for pacing
    pkts_per_sec = target_bps / bytes_per_pkt
    delay_per_pkt = 1.0 / pkts_per_sec if pkts_per_sec > 0 else 0

    start = time.time()
    seq = 0
    sent_bytes = 0

    with open(path, "rb") as f:
        # First chunk: meta + as much file content as fits
        body = f.read(first_data_cap)
        plen = len(meta) + len(body)
        pkt = MAGIC + struct.pack("<III", 0, total, plen) + meta + body
        s.sendto(pkt, (DST_IP, DST_PORT))
        sent_bytes += len(pkt)
        seq = 1

        # Remaining chunks: pure file content
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            pkt = MAGIC + struct.pack("<III", seq, total, len(chunk)) + chunk
            s.sendto(pkt, (DST_IP, DST_PORT))
            sent_bytes += len(pkt)
            seq += 1

            # Pace
            target_elapsed = seq * delay_per_pkt
            actual_elapsed = time.time() - start
            if target_elapsed > actual_elapsed:
                time.sleep(target_elapsed - actual_elapsed)

    elapsed = time.time() - start
    print(f"sent {seq} chunks, {sent_bytes:,} wire bytes, {size:,} file bytes")
    print(f"wall: {elapsed:.2f} s   = {size/elapsed/1024/1024:.1f} MB/s file payload")

if __name__ == "__main__":
    main()
