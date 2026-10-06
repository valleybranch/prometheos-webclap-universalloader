/*
 * Frames between the vstloader WebCLAP and its runtime (prometheos.runtime/1).
 * Every frame starts with a little-endian uint32 op. runtime/protocol.js is
 * the JavaScript twin.
 */
#ifndef VSTLOADER_PROTOCOL_H
#define VSTLOADER_PROTOCOL_H

#include <stdint.h>

enum {
    /* plugin -> runtime */
    VL_HELLO = 1,     /* vl_hello, then the plugin binary (dllSize bytes) */
    VL_SET_STATE = 2, /* the plugin's state (chunk) to restore */
    VL_GET_STATE = 3, /* asks for a fresh VL_STATE */
    VL_BYE = 4,       /* the instance is going away */
    VL_DEPENDENCY = 5, /* companion DLL frame; must precede VL_HELLO */

    /* runtime -> plugin */
    VL_STATE = 101,  /* the plugin's current state, cached for clap.state.save */
    VL_ERROR = 102,  /* UTF-8 text */
    VL_RESEND = 103, /* the runtime restarted: send VL_HELLO (and the state) again */
};

/* Where the instance's vstbridge channel lives in the module's memory, and
 * what to load into it. The runtime streams the channel at `channelBase`
 * (layout: vstbridge_abi.h, one channel). */
typedef struct vl_hello {
    uint32_t channelBase;  /* byte offset in the module's memory */
    uint32_t channelIndex; /* 0..VSTB_MAX_CHANNELS-1 within the module */
    uint32_t sampleRate;
    uint32_t blockFrames;
    uint32_t bridgeLatency; /* L, frames */
    uint32_t pluginLatency; /* the plugin's initialDelay at wrap time */
    uint32_t inPorts;
    uint32_t outPorts;
    uint32_t dllSize;
    char sha256[64]; /* the binary's SHA-256, hex */
    uint32_t reserved[3];
} vl_hello;

/* runtime/protocol.js reads it at these offsets (HELLO_BYTES = 112). */
_Static_assert(sizeof(vl_hello) == 112, "vl_hello is 112 bytes");

#endif
