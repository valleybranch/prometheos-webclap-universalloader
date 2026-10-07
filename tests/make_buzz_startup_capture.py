#!/usr/bin/env python3
"""Capture that exercises Buzz Work with warm-up before any explicit parameter event."""

import struct
import sys

RATE = 48000
BLOCK = 256
BLOCKS = 4
REQUEST_BYTES = 2048

def request(k):
    b = bytearray(REQUEST_BYTES)
    struct.pack_into("<IIII", b, 0, k, BLOCK, 0, 1)
    struct.pack_into("<dddd", b, 16, 126.0, k * BLOCK / RATE * 126.0 / 60.0,
                     float(k * BLOCK), 0.0)
    struct.pack_into("<IIII", b, 48, 0, 0, 0, 0)
    return b

def main(path):
    with open(path, "wb") as f:
        f.write(struct.pack("<4s7I", b"VSRP", 1, RATE, BLOCK, 0, 1, BLOCKS, 1))
        for k in range(BLOCKS):
            f.write(request(k))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "buzz-startup.capture")
