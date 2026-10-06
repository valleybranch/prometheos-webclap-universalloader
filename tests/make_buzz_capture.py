#!/usr/bin/env python3
"""Build a deterministic vsthost --replay capture for FSM Kick XP.

The first Buzz tick writes track 0 Trigger=128 (parameter 1), then Tick().
Subsequent blocks let the kick decay naturally. The capture uses the public
vstbridge_abi.h layout: a 32-byte VSRP header and 2048-byte requests.
"""
import struct
import sys

RATE = 48000
BLOCK = 256
BLOCKS = 96
REQUEST_BYTES = 2048
EVENTS_OFFSET = 64
EVENT_BYTES = 16

EV_BUZZ_VALUE = 16
EV_BUZZ_TICK = 17
EV_BUZZ_TRACKS = 18
EV_BUZZ_MASTER = 20

def event(offset, kind, index=0, value=0.0, midi=b"\0\0\0\0"):
    return struct.pack("<IHHf4s", offset, kind, index, float(value), midi)

def request(k):
    b = bytearray(REQUEST_BYTES)
    events = []
    # samples/tick for 126 BPM, 4 ticks/beat, matching the native host default.
    spt = RATE * 60 // (126 * 4)
    block_start = k * BLOCK
    # Put exact Buzz tick boundaries in the block. The first tick also triggers
    # track 0's Trigger/volume parameter at raw value 128.
    next_tick = ((block_start + spt - 1) // spt) * spt
    if k == 0:
        events += [
            event(0, EV_BUZZ_MASTER, 4, spt),
            event(0, EV_BUZZ_TRACKS, 0, 1),
            event(0, EV_BUZZ_VALUE, 1, 128, b"\x01\x00\x00\x00"),
            event(0, EV_BUZZ_TICK),
        ]
        next_tick = spt
    while next_tick < block_start + BLOCK:
        events.append(event(next_tick - block_start, EV_BUZZ_TICK))
        next_tick += spt

    # vstb_request fixed fields through eventCount/droppedEvents/reserved.
    struct.pack_into("<IIII", b, 0, k, BLOCK, 0, 1)
    struct.pack_into("<dddd", b, 16, 126.0, block_start / RATE * 126.0 / 60.0,
                     float(block_start), 0.0)
    struct.pack_into("<IIII", b, 48, len(events), 0, 0, 0)
    for i, ev in enumerate(events):
        b[EVENTS_OFFSET + i * EVENT_BYTES: EVENTS_OFFSET + (i + 1) * EVENT_BYTES] = ev
    return b

def main(path):
    with open(path, "wb") as f:
        f.write(struct.pack("<4s7I", b"VSRP", 1, RATE, BLOCK, 0, 1, BLOCKS, 0))
        for k in range(BLOCKS):
            f.write(request(k))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "fsm-kick-xp.capture")
