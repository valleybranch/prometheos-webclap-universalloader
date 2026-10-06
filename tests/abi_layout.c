// Prints the vstbridge layout (constants, struct sizes and field offsets) as
// JSON: the golden file include/vstbridge_abi.json that the JavaScript and
// TypeScript twins are tested against.
//   cc -std=c11 -Iinclude tests/abi_layout.c -o build/abi_layout && build/abi_layout
#include "vstbridge_abi.h"

#include <stdio.h>

static int first;
#define BEGIN(name) printf("%s  \"%s\": {", first++ ? ",\n" : "", name); int f_ = 0
#define FIELD(name, value) printf("%s\"%s\": %lu", f_++ ? ", " : "", name, (unsigned long)(value))
#define END() printf("}")
#define OFF(type, field) FIELD(#field, offsetof(type, field))

int main(void) {
    printf("{\n");
    {
        BEGIN("constants");
        FIELD("MAGIC", VSTB_MAGIC);
        FIELD("VERSION", VSTB_VERSION);
        FIELD("ATTACH_MAGIC", VSTB_ATTACH_MAGIC);
        FIELD("MAX_CHANNELS", VSTB_MAX_CHANNELS);
        FIELD("MAX_IN_PORTS", VSTB_MAX_IN_PORTS);
        FIELD("MAX_OUT_PORTS", VSTB_MAX_OUT_PORTS);
        FIELD("RING_FRAMES", VSTB_RING_FRAMES);
        FIELD("SLOTS", VSTB_SLOTS);
        FIELD("MAX_EVENTS", VSTB_MAX_EVENTS);
        FIELD("MIN_BLOCK_FRAMES", VSTB_MIN_BLOCK_FRAMES);
        FIELD("MAX_BLOCK_FRAMES", VSTB_MAX_BLOCK_FRAMES);
        FIELD("CTRL_BYTES", VSTB_CTRL_BYTES);
        FIELD("CTRL_OFFSET", VSTB_CTRL_OFFSET);
        FIELD("CTRL_REQUEST_OFFSET", VSTB_CTRL_REQUEST_OFFSET);
        FIELD("CTRL_RESPONSE_OFFSET", VSTB_CTRL_RESPONSE_OFFSET);
        FIELD("CHANNELS_OFFSET", VSTB_CHANNELS_OFFSET);
        FIELD("CHANNEL_BYTES", VSTB_CHANNEL_BYTES);
        FIELD("CHANNEL_CTL_BYTES", VSTB_CHANNEL_CTL_BYTES);
        FIELD("SLOTS_OFFSET", VSTB_SLOTS_OFFSET);
        FIELD("IN_RINGS_OFFSET", VSTB_IN_RINGS_OFFSET);
        FIELD("OUT_RINGS_OFFSET", VSTB_OUT_RINGS_OFFSET);
        FIELD("REQUEST_BYTES", VSTB_REQUEST_BYTES);
        FIELD("RESPONSE_HEADER_BYTES", VSTB_RESPONSE_HEADER_BYTES);
        FIELD("REGION_BYTES", VSTB_REGION_BYTES);
        FIELD("HIST_BUCKETS", VSTB_HIST_BUCKETS);
        END();
    }
    {
        BEGIN("enums");
        FIELD("STATE_FREE", VSTB_STATE_FREE);
        FIELD("STATE_LOADING", VSTB_STATE_LOADING);
        FIELD("STATE_READY", VSTB_STATE_READY);
        FIELD("STATE_FAULT", VSTB_STATE_FAULT);
        FIELD("STATE_CLOSED", VSTB_STATE_CLOSED);
        FIELD("BRIDGE_NONE", VSTB_BRIDGE_NONE);
        FIELD("BRIDGE_SERVING", VSTB_BRIDGE_SERVING);
        FIELD("EV_MIDI", VSTB_EV_MIDI);
        FIELD("EV_PARAM", VSTB_EV_PARAM);
        FIELD("EV_PROGRAM", VSTB_EV_PROGRAM);
        FIELD("REQ_PLAYING", VSTB_REQ_PLAYING);
        FIELD("REQ_KICK", VSTB_REQ_KICK);
        FIELD("RESP_IO_CHANGED", VSTB_RESP_IO_CHANGED);
        FIELD("OP_PING", VSTB_OP_PING);
        FIELD("OP_LOAD", VSTB_OP_LOAD);
        FIELD("OP_DESCRIBE", VSTB_OP_DESCRIBE);
        FIELD("OP_GET_STATE", VSTB_OP_GET_STATE);
        FIELD("OP_SET_STATE", VSTB_OP_SET_STATE);
        FIELD("OP_UNLOAD", VSTB_OP_UNLOAD);
        FIELD("OP_PUT_FILE", VSTB_OP_PUT_FILE);
        FIELD("OP_PROBE_IMPORTS", VSTB_OP_PROBE_IMPORTS);
        FIELD("OP_KICK", VSTB_OP_KICK);
        FIELD("STATUS_OK", VSTB_STATUS_OK);
        FIELD("STATUS_ERROR", VSTB_STATUS_ERROR);
        END();
    }
    {
        BEGIN("region_header");
        FIELD("size", sizeof(vstb_region_header));
        OFF(vstb_region_header, magic);
        OFF(vstb_region_header, version);
        OFF(vstb_region_header, regionBytes);
        OFF(vstb_region_header, maxChannels);
        OFF(vstb_region_header, channelsOffset);
        OFF(vstb_region_header, channelBytes);
        OFF(vstb_region_header, ringFrames);
        OFF(vstb_region_header, slots);
        OFF(vstb_region_header, bridgeState);
        OFF(vstb_region_header, bridgeAttaches);
        END();
    }
    {
        BEGIN("ctrl_header");
        FIELD("size", sizeof(vstb_ctrl_header));
        OFF(vstb_ctrl_header, requestSeq);
        OFF(vstb_ctrl_header, responseSeq);
        OFF(vstb_ctrl_header, guestWaiting);
        END();
    }
    {
        BEGIN("msg");
        FIELD("size", sizeof(vstb_msg));
        OFF(vstb_msg, seq);
        OFF(vstb_msg, op);
        OFF(vstb_msg, channel);
        OFF(vstb_msg, length);
        OFF(vstb_msg, status);
        END();
    }
    {
        BEGIN("attach");
        FIELD("size", sizeof(vstb_attach));
        OFF(vstb_attach, magic);
        OFF(vstb_attach, channel);
        OFF(vstb_attach, sampleRate);
        OFF(vstb_attach, blockFrames);
        OFF(vstb_attach, inPorts);
        OFF(vstb_attach, outPorts);
        OFF(vstb_attach, pluginLatency);
        END();
    }
    {
        BEGIN("channel_ctl");
        FIELD("size", sizeof(vstb_channel_ctl));
        OFF(vstb_channel_ctl, state);
        OFF(vstb_channel_ctl, doorbell);
        OFF(vstb_channel_ctl, requestSeq);
        OFF(vstb_channel_ctl, responseSeq);
        OFF(vstb_channel_ctl, kickSeq);
        OFF(vstb_channel_ctl, generation);
        OFF(vstb_channel_ctl, sampleRate);
        OFF(vstb_channel_ctl, blockFrames);
        OFF(vstb_channel_ctl, inPorts);
        OFF(vstb_channel_ctl, outPorts);
        OFF(vstb_channel_ctl, pluginLatency);
        OFF(vstb_channel_ctl, underruns);
        OFF(vstb_channel_ctl, skipped);
        OFF(vstb_channel_ctl, lastProcessUs);
        OFF(vstb_channel_ctl, maxProcessUs);
        OFF(vstb_channel_ctl, fault);
        OFF(vstb_channel_ctl, attached);
        OFF(vstb_channel_ctl, turnUs);
        OFF(vstb_channel_ctl, wakeHist);
        OFF(vstb_channel_ctl, turnHist);
        OFF(vstb_channel_ctl, doneBlock);
        END();
    }
    {
        BEGIN("event");
        FIELD("size", sizeof(vstb_event));
        OFF(vstb_event, offset);
        OFF(vstb_event, type);
        OFF(vstb_event, index);
        OFF(vstb_event, value);
        OFF(vstb_event, midi);
        END();
    }
    {
        BEGIN("request");
        FIELD("size", sizeof(vstb_request));
        OFF(vstb_request, blockIndex);
        OFF(vstb_request, frames);
        OFF(vstb_request, connectedMask);
        OFF(vstb_request, flags);
        OFF(vstb_request, tempo);
        OFF(vstb_request, ppqPos);
        OFF(vstb_request, samplePos);
        OFF(vstb_request, hostTimeMs);
        OFF(vstb_request, eventCount);
        OFF(vstb_request, droppedEvents);
        OFF(vstb_request, events);
        END();
    }
    {
        BEGIN("response");
        FIELD("size", sizeof(vstb_response));
        OFF(vstb_response, blockIndex);
        OFF(vstb_response, frames);
        OFF(vstb_response, processUs);
        OFF(vstb_response, status);
        END();
    }
    printf("\n}\n");
    return 0;
}
