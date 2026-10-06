#pragma once

/*
 * vstbridge: the shared-memory layout of Boxedwine's /dev/vstbridge device.
 *
 * One region of WebAssembly memory (a SharedArrayBuffer in the browser) is
 * shared by three parties:
 *   - the host page / AudioWorklet (JavaScript, through Atomics on the region),
 *   - the device in Boxedwine (C++, compiled to WebAssembly), and
 *   - vsthost.exe --bridge under Wine (i686 Windows code), which only ever sees
 *     the records below through ReadFile/WriteFile on Z:\dev\vstbridge.
 *
 * This header is the single source of truth. The same file is compiled by
 * Emscripten (Boxedwine patch 0003 carries a copy), MinGW (vsthost) and the
 * native layout test (tests/abi_layout.c), which writes vstbridge_abi.json;
 * the JavaScript and TypeScript twins are tested against that JSON.
 *
 * Everything is little-endian. Doubles are placed at 8-byte offsets so that
 * wasm32, MinGW i686 (8-byte double alignment) and System V i386 (4-byte)
 * agree; the static asserts at the end check the offsets on every compiler.
 *
 * Region (offsets from the region base):
 *   0                  vstb_region_header (256 bytes)
 *   VSTB_CTRL_OFFSET   vstb_ctrl_header (256 bytes), then the request message
 *                      and the response message (VSTB_CTRL_BYTES each)
 *   VSTB_CHANNELS_OFFSET + (n - 1) * VSTB_CHANNEL_BYTES
 *                      audio channel n (1..VSTB_MAX_CHANNELS):
 *                        vstb_channel_ctl (VSTB_CHANNEL_CTL_BYTES, padded to 4096)
 *                        request slots    (VSTB_SLOTS x VSTB_REQUEST_BYTES)
 *                        input rings      (VSTB_MAX_IN_PORTS x 2 x VSTB_RING_FRAMES floats)
 *                        output rings     (VSTB_MAX_OUT_PORTS x 2 x VSTB_RING_FRAMES floats)
 *
 * Audio protocol (one channel = one plugin instance):
 *   host:  for block k (B = blockFrames frames), write the request into slot
 *          k % VSTB_SLOTS and the inputs into the input rings at frame
 *          (k * B) % VSTB_RING_FRAMES (port p, channel c: ring 2p + c), then
 *          store requestSeq = k + 1, add 1 to doorbell and Atomics.notify it.
 *   guest: ReadFile returns vstb_request followed by the inputs (planar
 *          float32, inPorts x 2 x B). WriteFile takes vstb_response followed
 *          by the outputs (outPorts x 2 x B); the device copies them into the
 *          output rings at frame (k * B) % VSTB_RING_FRAMES and stores
 *          responseSeq = k + 1.
 *   host:  plays output frame t - L at time t from block b = (t - L) / B once
 *          doneBlock[b % VSTB_SLOTS] == b + 1 (responseSeq alone would also
 *          pass blocks the guest skipped). Otherwise it plays silence and
 *          counts an underrun.
 * A read that is woken by kickSeq instead returns only a vstb_request with
 * frames = 0 and VSTB_REQ_KICK set, so the instance thread can run a command.
 * Guest buffers must be page-aligned and audio reads must ask for exactly
 * VSTB_REQUEST_BYTES + input bytes (Boxedwine copies page by page).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VSTB_MAGIC 0x42545356u /* 'VSTB' */
#define VSTB_VERSION 1u
#define VSTB_ATTACH_MAGIC 0x48435441u /* 'ATCH' */

#define VSTB_MAX_CHANNELS 8u     /* audio channels 1..8; 0 is the control channel */
#define VSTB_MAX_IN_PORTS 4u     /* stereo input ports per channel */
#define VSTB_MAX_OUT_PORTS 4u    /* stereo output ports per channel */
#define VSTB_RING_FRAMES 8192u   /* frames per audio ring (power of two) */
#define VSTB_SLOTS 64u           /* request slots per channel (power of two) */
#define VSTB_MAX_EVENTS 120u     /* events per request */
#define VSTB_MIN_BLOCK_FRAMES 32u
#define VSTB_MAX_BLOCK_FRAMES 1024u
#define VSTB_CTRL_BYTES (2u << 20) /* capacity of each control message, header included */

#define VSTB_REGION_HEADER_BYTES 256u
#define VSTB_CTRL_OFFSET 4096u
#define VSTB_CTRL_HEADER_BYTES 256u
#define VSTB_CTRL_REQUEST_OFFSET (VSTB_CTRL_OFFSET + VSTB_CTRL_HEADER_BYTES)
#define VSTB_CTRL_RESPONSE_OFFSET (VSTB_CTRL_REQUEST_OFFSET + VSTB_CTRL_BYTES)
#define VSTB_CHANNELS_OFFSET (VSTB_CTRL_RESPONSE_OFFSET + VSTB_CTRL_BYTES + 3840u) /* 4096-aligned */
#define VSTB_CHANNEL_CTL_BYTES 1024u
#define VSTB_REQUEST_BYTES 2048u
#define VSTB_RESPONSE_HEADER_BYTES 32u
#define VSTB_SLOTS_OFFSET 4096u /* within a channel; vstb_channel_ctl comes first */
#define VSTB_IN_RINGS_OFFSET (VSTB_SLOTS_OFFSET + VSTB_SLOTS * VSTB_REQUEST_BYTES)
#define VSTB_OUT_RINGS_OFFSET (VSTB_IN_RINGS_OFFSET + VSTB_MAX_IN_PORTS * 2u * VSTB_RING_FRAMES * 4u)
#define VSTB_CHANNEL_BYTES (VSTB_OUT_RINGS_OFFSET + VSTB_MAX_OUT_PORTS * 2u * VSTB_RING_FRAMES * 4u)
#define VSTB_REGION_BYTES (VSTB_CHANNELS_OFFSET + VSTB_MAX_CHANNELS * VSTB_CHANNEL_BYTES)

/* vstb_channel_ctl.state */
enum {
    VSTB_STATE_FREE = 0,
    VSTB_STATE_LOADING = 1, /* LOAD sent, plugin not attached yet */
    VSTB_STATE_READY = 2,   /* instance thread attached; requests are served */
    VSTB_STATE_FAULT = 3,
    VSTB_STATE_CLOSED = 4,
};

/* vstb_region_header.bridgeState */
enum { VSTB_BRIDGE_NONE = 0, VSTB_BRIDGE_SERVING = 1 };

/* vstb_event.type */
enum {
    VSTB_EV_MIDI = 1,    /* midi[0..2]: a short MIDI message */
    VSTB_EV_PARAM = 2,   /* index: parameter, value: normalized 0..1 */
    VSTB_EV_PROGRAM = 3, /* index: program number */
    VSTB_EV_BUZZ_VALUE = 16,  /* index: parameter, value: raw integer; midi[0..1]: track+1, 0=global */
    VSTB_EV_BUZZ_TICK = 17,   /* offset: exact Buzz tick boundary */
    VSTB_EV_BUZZ_TRACKS = 18, /* value: active track count */
    VSTB_EV_BUZZ_ATTR = 19,   /* index: attribute, value: raw integer */
    VSTB_EV_BUZZ_MASTER = 20, /* index: ticks/beat, value: exact samples/tick */
    VSTB_EV_BUZZ_STOP = 21,
};

/* vstb_request.flags */
enum {
    VSTB_REQ_PLAYING = 1u << 0, /* transport playing */
    VSTB_REQ_KICK = 1u << 1,    /* not audio: run the instance's pending command */
};

/* vstb_response.status */
enum {
    VSTB_RESP_IO_CHANGED = 1u << 0, /* the plugin reported new latency or I/O (audioMasterIOChanged):
                                       the host re-describes and applies it at its next restart */
};

/* vstb_msg.op: control requests (host -> guest) */
enum {
    VSTB_OP_PING = 1,      /* -> "pong" */
    VSTB_OP_LOAD = 2,      /* text "rate=<hz> block=<frames> warmup=<0|1> path=<windows path>" -> describe JSON */
    VSTB_OP_DESCRIBE = 3,  /* -> describe JSON */
    VSTB_OP_GET_STATE = 4, /* -> vstb state blob (see vsthost) */
    VSTB_OP_SET_STATE = 5, /* vstb state blob -> "" */
    VSTB_OP_UNLOAD = 6,    /* -> "" */
    VSTB_OP_PUT_FILE = 7,  /* u32 offset, u32 total, u32 pathBytes, path, bytes -> "": writes a guest file
                              in pieces (files the page writes into Emscripten's FS are invisible to Wine) */
    VSTB_OP_KICK = 100,    /* guest only: written on the control handle, wakes channel's instance thread */
};

/* vstb_msg.status */
enum { VSTB_STATUS_OK = 0, VSTB_STATUS_ERROR = 1 };

typedef struct vstb_region_header {
    uint32_t magic;          /* VSTB_MAGIC */
    uint32_t version;        /* VSTB_VERSION */
    uint32_t regionBytes;    /* VSTB_REGION_BYTES */
    uint32_t maxChannels;    /* VSTB_MAX_CHANNELS */
    uint32_t channelsOffset; /* VSTB_CHANNELS_OFFSET */
    uint32_t channelBytes;   /* VSTB_CHANNEL_BYTES */
    uint32_t ringFrames;     /* VSTB_RING_FRAMES */
    uint32_t slots;          /* VSTB_SLOTS */
    int32_t bridgeState;     /* VSTB_BRIDGE_*: set when vsthost attaches the control channel */
    int32_t bridgeAttaches;  /* control attaches so far */
    uint32_t reserved[54];
} vstb_region_header;

typedef struct vstb_ctrl_header {
    int32_t requestSeq;   /* host: bumped (and notified) after writing the request message */
    int32_t responseSeq;  /* guest: set to the answered request's seq (and notified) */
    int32_t guestWaiting; /* 1 while the control thread waits for a request */
    int32_t reserved[61];
} vstb_ctrl_header;

/* Header of a control message; `length` payload bytes follow. */
typedef struct vstb_msg {
    uint32_t seq;     /* request: ctrl requestSeq value; response: same */
    uint32_t op;      /* VSTB_OP_* */
    uint32_t channel; /* audio channel the request concerns, 0 if none */
    uint32_t length;  /* payload bytes */
    int32_t status;   /* response: VSTB_STATUS_* */
    uint32_t reserved[3];
} vstb_msg;

/* The first write on a fresh handle binds it to a channel. */
typedef struct vstb_attach {
    uint32_t magic;   /* VSTB_ATTACH_MAGIC */
    uint32_t channel; /* 0: control; 1..VSTB_MAX_CHANNELS: audio */
    /* audio channels: the instance's configuration, published in vstb_channel_ctl */
    uint32_t sampleRate;
    uint32_t blockFrames;
    uint32_t inPorts;
    uint32_t outPorts;
    uint32_t pluginLatency;
    uint32_t reserved;
} vstb_attach;

#define VSTB_HIST_BUCKETS 16u /* bucket i counts values in [2^i, 2^(i+1)) microseconds; 0 in bucket 0 */

/* Per-channel control words. Int32 words, read and written with atomics. */
typedef struct vstb_channel_ctl {
    int32_t state;         /* VSTB_STATE_* */
    int32_t doorbell;      /* futex word the instance thread waits on */
    int32_t requestSeq;    /* requests published by the host */
    int32_t responseSeq;   /* responses written by the guest */
    int32_t kickSeq;       /* commands for the instance thread (doorbell rung too) */
    int32_t generation;    /* bumped at every attach: a new instance, sequences restart at 0 */
    int32_t sampleRate;    /* from vstb_attach */
    int32_t blockFrames;
    int32_t inPorts;
    int32_t outPorts;
    int32_t pluginLatency;
    int32_t underruns;     /* host: blocks played as silence */
    int32_t skipped;       /* guest: requests skipped because it fell VSTB_SLOTS behind */
    int32_t lastProcessUs; /* guest: plugin processing time of the last block (guest clock: 1 ms steps in Boxedwine) */
    int32_t maxProcessUs;
    int32_t fault;
    int32_t attached;      /* handles bound to this channel */
    int32_t reserved0[15];
    int32_t turnUs[VSTB_SLOTS];             /* device: block k's request delivered -> response written, us */
    int32_t wakeHist[VSTB_HIST_BUCKETS];    /* device: publish -> guest wake-up (requests with hostTimeMs) */
    int32_t turnHist[VSTB_HIST_BUCKETS];    /* device: request delivered -> response written */
    int32_t doneBlock[VSTB_SLOTS];          /* device: k + 1 once block k's outputs are in the rings */
    int32_t reserved1[64];
} vstb_channel_ctl;

typedef struct vstb_event {
    uint32_t offset; /* frame within the block */
    uint16_t type;   /* VSTB_EV_* */
    uint16_t index;  /* parameter index or program */
    float value;     /* parameter value 0..1 */
    uint8_t midi[4]; /* VSTB_EV_MIDI: status, data1, data2, 0 */
} vstb_event;

typedef struct vstb_request {
    uint32_t blockIndex;    /* k */
    uint32_t frames;        /* B, or 0 for a kick */
    uint32_t connectedMask; /* bit p: input port p is connected (else silence) */
    uint32_t flags;         /* VSTB_REQ_* */
    double tempo;           /* BPM */
    double ppqPos;          /* quarter notes at the first frame */
    double samplePos;       /* song frames at the first frame */
    double hostTimeMs;      /* publish time (performance.timeOrigin + now()), 0 if unknown */
    uint32_t eventCount;
    uint32_t droppedEvents; /* events that did not fit */
    uint32_t reserved[2];
    vstb_event events[VSTB_MAX_EVENTS];
    uint8_t pad[64];
} vstb_request;

typedef struct vstb_response {
    uint32_t blockIndex;
    uint32_t frames;
    uint32_t processUs; /* plugin processing time */
    uint32_t status;    /* VSTB_RESP_* */
    uint32_t reserved[4];
} vstb_response;

#if defined(__cplusplus)
#define VSTB_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define VSTB_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

VSTB_STATIC_ASSERT(sizeof(vstb_region_header) == VSTB_REGION_HEADER_BYTES, "region header size");
VSTB_STATIC_ASSERT(sizeof(vstb_ctrl_header) == VSTB_CTRL_HEADER_BYTES, "ctrl header size");
VSTB_STATIC_ASSERT(sizeof(vstb_msg) == 32, "msg size");
VSTB_STATIC_ASSERT(sizeof(vstb_attach) == 32, "attach size");
VSTB_STATIC_ASSERT(sizeof(vstb_channel_ctl) == VSTB_CHANNEL_CTL_BYTES, "channel ctl size");
VSTB_STATIC_ASSERT(sizeof(vstb_event) == 16, "event size");
VSTB_STATIC_ASSERT(sizeof(vstb_request) == VSTB_REQUEST_BYTES, "request size");
VSTB_STATIC_ASSERT(sizeof(vstb_response) == VSTB_RESPONSE_HEADER_BYTES, "response size");
VSTB_STATIC_ASSERT(offsetof(vstb_request, tempo) == 16, "request.tempo");
VSTB_STATIC_ASSERT(offsetof(vstb_request, hostTimeMs) == 40, "request.hostTimeMs");
VSTB_STATIC_ASSERT(offsetof(vstb_request, eventCount) == 48, "request.eventCount");
VSTB_STATIC_ASSERT(offsetof(vstb_request, events) == 64, "request.events");
VSTB_STATIC_ASSERT(offsetof(vstb_channel_ctl, turnUs) == 128, "ctl.turnUs");
VSTB_STATIC_ASSERT(VSTB_CHANNEL_CTL_BYTES <= VSTB_SLOTS_OFFSET, "ctl fits before the slots");
VSTB_STATIC_ASSERT(VSTB_CHANNELS_OFFSET % 4096u == 0, "channels 4096-aligned");
VSTB_STATIC_ASSERT(VSTB_CHANNEL_BYTES % 4096u == 0, "channel stride 4096-aligned");
VSTB_STATIC_ASSERT((VSTB_RING_FRAMES & (VSTB_RING_FRAMES - 1u)) == 0, "ring frames power of two");
VSTB_STATIC_ASSERT((VSTB_SLOTS & (VSTB_SLOTS - 1u)) == 0, "slots power of two");

#ifdef __cplusplus
}
#endif
