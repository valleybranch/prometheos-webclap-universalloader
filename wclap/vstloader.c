/*
 * vstloader: a WebCLAP that plays a 32-bit Windows VST2 plugin running under
 * Wine inside Boxedwine (the "runtime", a separate page the host loads).
 *
 * The plugin's DSP runs on an emulator thread. This module only streams it:
 * every B frames become one request (MIDI, parameter changes, the input of an
 * effect) in a vstbridge channel that lives in this module's own shared
 * memory, and the output played at frame t is the plugin's output for frame
 * t - L. The runtime's relay moves each request into Boxedwine's
 * /dev/vstbridge and each answered block back, so audio never touches the
 * browser's main thread. clap.latency reports L plus the plugin's own delay,
 * so the host's delay compensation keeps an effect aligned with its dry path.
 *
 * A wrapped bundle carries resources/vstloader.txt (the descriptor frozen when
 * the .dll was wrapped) and resources/plugin.dll. Until the runtime has loaded
 * the plugin the output is silence. process() does not allocate.
 *
 * The channel layout and the streaming rules are vstbridge_abi.h's: this
 * module plays the part the AudioWorklet plays against the device.
 */
#include <clap/clap.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prometheos_runtime.h"
#include "vstbridge_abi.h"
#include "vstloader_protocol.h"

#define MAX_PARAMS 1024
#define MAX_DEPENDENCIES 64
#define MAX_PORTS 4
#define NAME_BYTES 256
#define STATE_REFRESH_BLOCKS 94
#define MAX_BUZZ_PENDING 120
#define PROMETHEOS_BUZZ_MACHINE_EXT "prometheos.buzz-machine/1" /* ~0.5 s of 256-frame blocks at 48 kHz */

/* ---- atomics on the shared channel ------------------------------------------ */

static inline int32_t load32(const int32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void store32(int32_t *p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline void ring(int32_t *p) {
    __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify(p, 0x7fffffff);
}

/* ---- the frozen descriptor ---------------------------------------------------- */

typedef struct {
    char name[NAME_BYTES];
    char label[NAME_BYTES];
    double defaultValue;
} vl_param_info;

typedef struct {
    char name[NAME_BYTES];
    char sha256[65];
    char resource[1024];
    uint8_t *bytes;
    uint32_t size;
} vl_dependency_info;

typedef struct {
    char id[NAME_BYTES];
    char name[NAME_BYTES];
    char vendor[NAME_BYTES];
    char version[64];
    char sha256[65];
    char runtime[1024];
    char dll[NAME_BYTES];
    char format[32];
    char buzzLayout[65536];
    bool synth;
    uint32_t inPorts, outPorts;
    uint32_t pluginLatency;
    uint32_t bridgeLatency;
    uint32_t blockFrames;
    uint32_t paramCount;
    vl_param_info *params;
    uint32_t dependencyCount;
    vl_dependency_info dependencies[MAX_DEPENDENCIES];
} vl_descriptor;

static vl_descriptor g_desc;
static char g_bundle[1024];
static const char *g_features_synth[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, NULL};
static const char *g_features_effect[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static clap_plugin_descriptor_t g_clap_desc;

static int asciiCaseCmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ac = (unsigned char)*a++, bc = (unsigned char)*b++;
        if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + ('a' - 'A'));
        if (bc >= 'A' && bc <= 'Z') bc = (unsigned char)(bc + ('a' - 'A'));
        if (ac != bc || !ac || !bc) return (int)ac - (int)bc;
    }
}

static void copyField(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* resources/vstloader.txt: "key=value" lines; "param=name<TAB>label<TAB>default" in order. */
static bool readDescriptor(const char *bundle) {
    char path[1200];
    snprintf(path, sizeof path, "%s/resources/vstloader.txt", bundle);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.params = calloc(MAX_PARAMS, sizeof(vl_param_info));
    g_desc.outPorts = 1;
    g_desc.bridgeLatency = 2048;
    g_desc.blockFrames = 256;
    copyField(g_desc.dll, sizeof g_desc.dll, "plugin.dll");
    copyField(g_desc.version, sizeof g_desc.version, "1.0.0");
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line, *value = eq + 1;
        if (!strcmp(key, "id")) copyField(g_desc.id, sizeof g_desc.id, value);
        else if (!strcmp(key, "name")) copyField(g_desc.name, sizeof g_desc.name, value);
        else if (!strcmp(key, "vendor")) copyField(g_desc.vendor, sizeof g_desc.vendor, value);
        else if (!strcmp(key, "version")) copyField(g_desc.version, sizeof g_desc.version, value);
        else if (!strcmp(key, "sha256")) copyField(g_desc.sha256, sizeof g_desc.sha256, value);
        else if (!strcmp(key, "runtime")) copyField(g_desc.runtime, sizeof g_desc.runtime, value);
        else if (!strcmp(key, "dll")) copyField(g_desc.dll, sizeof g_desc.dll, value);
        else if (!strcmp(key, "format")) copyField(g_desc.format, sizeof g_desc.format, value);
        else if (!strcmp(key, "buzzLayout")) copyField(g_desc.buzzLayout, sizeof g_desc.buzzLayout, value);
        else if (!strcmp(key, "synth")) g_desc.synth = atoi(value) != 0;
        else if (!strcmp(key, "inPorts")) g_desc.inPorts = (uint32_t)atoi(value);
        else if (!strcmp(key, "outPorts")) g_desc.outPorts = (uint32_t)atoi(value);
        else if (!strcmp(key, "latency")) g_desc.pluginLatency = (uint32_t)atoi(value);
        else if (!strcmp(key, "bridgeLatency")) g_desc.bridgeLatency = (uint32_t)atoi(value);
        else if (!strcmp(key, "block")) g_desc.blockFrames = (uint32_t)atoi(value);
        else if (!strcmp(key, "dependency")) {
            if (g_desc.dependencyCount >= MAX_DEPENDENCIES) { fclose(f); return false; }
            char *tab1 = strchr(value, '\t');
            char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
            if (!tab1 || !tab2) { fclose(f); return false; }
            *tab1 = 0; *tab2 = 0;
            if (!value[0] || strchr(value, '/') || strchr(value, '\\') || strchr(value, ':') ||
                strlen(value) < 4 || asciiCaseCmp(value + strlen(value) - 4, ".dll") || !asciiCaseCmp(value, "plugin.dll") ||
                strlen(tab1 + 1) != 64 || strncmp(tab2 + 1, "resources/deps/", 15)) { fclose(f); return false; }
            for (uint32_t i = 0; i < g_desc.dependencyCount; ++i)
                if (!asciiCaseCmp(g_desc.dependencies[i].name, value)) { fclose(f); return false; }
            vl_dependency_info *d = &g_desc.dependencies[g_desc.dependencyCount++];
            copyField(d->name, sizeof d->name, value);
            copyField(d->sha256, sizeof d->sha256, tab1 + 1);
            copyField(d->resource, sizeof d->resource, tab2 + 1);
        }
        else if (!strcmp(key, "param") && g_desc.paramCount < MAX_PARAMS) {
            vl_param_info *p = &g_desc.params[g_desc.paramCount++];
            char *tab1 = strchr(value, '\t');
            char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
            if (tab1) *tab1 = 0;
            if (tab2) *tab2 = 0;
            copyField(p->name, sizeof p->name, value);
            copyField(p->label, sizeof p->label, tab1 ? tab1 + 1 : "");
            p->defaultValue = tab2 ? strtod(tab2 + 1, NULL) : 0.0;
            if (!(p->defaultValue >= 0.0 && p->defaultValue <= 1.0)) p->defaultValue = 0.0;
        }
    }
    fclose(f);
    if (g_desc.inPorts > MAX_PORTS) g_desc.inPorts = MAX_PORTS;
    if (g_desc.outPorts > MAX_PORTS) g_desc.outPorts = MAX_PORTS;
    if (g_desc.outPorts < 1) g_desc.outPorts = 1;
    if (g_desc.blockFrames == 0 || g_desc.blockFrames > VSTB_RING_FRAMES / 8) g_desc.blockFrames = 256;
    return g_desc.id[0] != 0 && g_desc.name[0] != 0;
}

static bool loadDependencies(const char *bundle) {
    for (uint32_t i = 0; i < g_desc.dependencyCount; ++i) {
        vl_dependency_info *d = &g_desc.dependencies[i];
        char path[2200];
        snprintf(path, sizeof path, "%s/%s", bundle, d->resource);
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        if (fseek(f, 0, SEEK_END) || (long)(d->size = (uint32_t)ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return false; }
        d->bytes = d->size ? malloc(d->size) : NULL;
        if (!d->bytes || fread(d->bytes, 1, d->size, f) != d->size) { fclose(f); return false; }
        fclose(f);
    }
    return true;
}

/* ---- channels: VSTB_MAX_CHANNELS of them in this module's memory --------------- */

static uint8_t g_channels[VSTB_MAX_CHANNELS][VSTB_CHANNEL_BYTES] __attribute__((aligned(4096)));
static bool g_channelUsed[VSTB_MAX_CHANNELS];

/* ---- the plugin ---------------------------------------------------------------- */

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const prometheos_host_runtime_t *hostRuntime;
    const clap_host_log_t *hostLog;

    int channel; /* 0..VSTB_MAX_CHANNELS-1, -1 if none */
    vstb_channel_ctl *ctl;
    uint8_t *slots;
    float *inRings[MAX_PORTS * 2];
    float *outRings[MAX_PORTS * 2];

    double sampleRate;
    bool active;
    bool helloSent;
    int32_t seenGeneration;
    bool attached;
    uint32_t B, L;
    uint32_t k;          /* block being assembled */
    int64_t frame;       /* stream frame of the next process() call */
    int64_t startFrame;
    uint32_t connectedThisBlock;
    int64_t lastUnderrunBlock;
    vstb_request request;

    bool releaseAll;
    double *paramValues;
    uint32_t pendingParams[MAX_PARAMS];
    float pendingValues[MAX_PARAMS];
    uint32_t pendingCount;

    struct { int16_t track; uint16_t index; int32_t value; } buzzValues[MAX_BUZZ_PENDING];
    uint32_t buzzValueCount;
    int32_t buzzTracks;
    bool buzzTracksDirty;
    uint16_t buzzAttrIndex[MAX_BUZZ_PENDING];
    int32_t buzzAttrValue[MAX_BUZZ_PENDING];
    uint32_t buzzAttrCount;
    int32_t buzzBpm, buzzTpb;
    double buzzSpt;
    bool buzzMasterDirty;
    bool buzzStopPending;

    uint8_t *state;
    uint32_t stateSize;
    bool stateToSend;
    /* Parameters changed since the runtime last reported the state (the chunk
     * clap.state.save hands out is the runtime's last report). */
    bool stateDirty;
    bool callbackPending;
    uint32_t lastStateRequestBlock;
    uint32_t underrunBlocks;
} vl_plugin;

static vl_plugin *self(const clap_plugin_t *p) { return (vl_plugin *)p->plugin_data; }

static void logf_(vl_plugin *v, clap_log_severity severity, const char *message) {
    if (v->hostLog) v->hostLog->log(v->host, severity, message);
}

static bool sendFrame(vl_plugin *v, uint32_t op, const void *a, uint32_t aSize, const void *b, uint32_t bSize) {
    if (!v->hostRuntime) return false;
    uint32_t size = 4 + aSize + bSize;
    uint8_t *frame = malloc(size);
    if (!frame) return false;
    memcpy(frame, &op, 4);
    if (aSize) memcpy(frame + 4, a, aSize);
    if (bSize) memcpy(frame + 4 + aSize, b, bSize);
    bool ok = v->hostRuntime->send(v->host, frame, size);
    free(frame);
    return ok;
}

static bool sendDependencies(vl_plugin *v) {
    for (uint32_t i = 0; i < g_desc.dependencyCount; ++i) {
        const vl_dependency_info *d = &g_desc.dependencies[i];
        const uint16_t nameBytes = (uint16_t)strlen(d->name);
        const uint32_t headerBytes = 40;
        const uint32_t bodySize = headerBytes + nameBytes + d->size;
        uint8_t *body = malloc(bodySize);
        if (!body) return false;
        memset(body, 0, headerBytes);
        memcpy(body, &nameBytes, 2);
        memcpy(body + 4, &d->size, 4);
        for (uint32_t j = 0; j < 32; ++j) {
            unsigned value = 0;
            if (sscanf(d->sha256 + j * 2, "%2x", &value) != 1) { free(body); return false; }
            body[8 + j] = (uint8_t)value;
        }
        memcpy(body + headerBytes, d->name, nameBytes);
        memcpy(body + headerBytes + nameBytes, d->bytes, d->size);
        bool ok = sendFrame(v, VL_DEPENDENCY, body, bodySize, NULL, 0);
        free(body);
        if (!ok) return false;
    }
    return true;
}

/* Reads resources/<dll> and hands it, with the channel's whereabouts, to the runtime. */
static void sendHello(vl_plugin *v) {
    if (v->helloSent || !v->hostRuntime || v->channel < 0) return;
    char path[1400];
    snprintf(path, sizeof path, "%s/resources/%s", g_bundle, g_desc.dll);
    FILE *f = fopen(path, "rb");
    if (!f) {
        logf_(v, CLAP_LOG_ERROR, "vstloader: the bundle has no plugin binary");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *dll = size > 0 ? malloc((size_t)size) : NULL;
    if (!dll || fread(dll, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(dll);
        logf_(v, CLAP_LOG_ERROR, "vstloader: cannot read the plugin binary");
        return;
    }
    fclose(f);
    vl_hello hello;
    memset(&hello, 0, sizeof hello);
    hello.channelBase = (uint32_t)(uintptr_t)g_channels[v->channel];
    hello.channelIndex = (uint32_t)v->channel;
    hello.sampleRate = (uint32_t)lround(v->sampleRate);
    hello.blockFrames = g_desc.blockFrames;
    hello.bridgeLatency = g_desc.bridgeLatency;
    hello.pluginLatency = g_desc.pluginLatency;
    hello.inPorts = g_desc.inPorts;
    hello.outPorts = g_desc.outPorts;
    hello.dllSize = (uint32_t)size;
    memcpy(hello.sha256, g_desc.sha256, 64);
    if (!sendDependencies(v)) {
        free(dll);
        logf_(v, CLAP_LOG_ERROR, "vstloader: cannot send companion dependencies");
        return;
    }
    v->helloSent = sendFrame(v, VL_HELLO, &hello, sizeof hello, dll, (uint32_t)size);
    free(dll);
    if (v->helloSent && v->stateToSend && v->state) {
        v->stateToSend = !sendFrame(v, VL_SET_STATE, v->state, v->stateSize, NULL, 0);
    }
}

/* Follows the runtime: once it bumps the channel's generation, the stream (re)starts. */
static bool attached(vl_plugin *v) {
    if (!v->ctl) return false;
    int32_t generation = load32(&v->ctl->generation);
    if (generation == v->seenGeneration) return v->attached;
    v->seenGeneration = generation;
    v->attached = generation > 0 && load32(&v->ctl->state) == VSTB_STATE_READY;
    if (!v->attached) return false;
    v->B = (uint32_t)load32(&v->ctl->blockFrames);
    if (v->B == 0) v->B = g_desc.blockFrames;
    v->L = g_desc.bridgeLatency;
    v->k = (uint32_t)load32(&v->ctl->requestSeq);
    v->frame = (int64_t)v->k * v->B;
    v->startFrame = v->frame;
    v->connectedThisBlock = 0;
    v->lastUnderrunBlock = -1;
    memset(&v->request, 0, sizeof v->request);
    return true;
}

static void addEvent(vl_plugin *v, uint32_t offset, uint16_t type, uint16_t index, float value, uint8_t s, uint8_t d1, uint8_t d2) {
    vstb_request *r = &v->request;
    if (r->eventCount >= VSTB_MAX_EVENTS) {
        r->droppedEvents++;
        return;
    }
    vstb_event *e = &r->events[r->eventCount++];
    e->offset = offset;
    e->type = type;
    e->index = index;
    e->value = value;
    e->midi[0] = s;
    e->midi[1] = d1;
    e->midi[2] = d2;
    e->midi[3] = 0;
}

static void midiEvent(vl_plugin *v, uint32_t offset, uint8_t s, uint8_t d1, uint8_t d2) {
    addEvent(v, offset, VSTB_EV_MIDI, 0, 0.0f, s, d1, d2);
}

static void paramEvent(vl_plugin *v, uint32_t offset, uint32_t index, double value) {
    if (index >= g_desc.paramCount) return;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    v->paramValues[index] = value;
    v->stateDirty = true;
    addEvent(v, offset, VSTB_EV_PARAM, (uint16_t)index, (float)value, 0, 0, 0);
}

static void applyEvent(vl_plugin *v, const clap_event_header_t *h, uint32_t offset) {
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *n = (const clap_event_note_t *)h;
        if (n->key < 0 || n->key > 127) break;
        uint8_t channel = (uint8_t)((n->channel < 0 ? 0 : n->channel) & 0x0f);
        if (h->type == CLAP_EVENT_NOTE_ON) {
            long velocity = lround(n->velocity * 127.0);
            if (velocity < 1) velocity = 1;
            if (velocity > 127) velocity = 127;
            midiEvent(v, offset, 0x90 | channel, (uint8_t)n->key, (uint8_t)velocity);
        } else {
            midiEvent(v, offset, 0x80 | channel, (uint8_t)n->key, 0);
        }
        break;
    }
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *m = (const clap_event_midi_t *)h;
        midiEvent(v, offset, m->data[0], m->data[1], m->data[2]);
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *p = (const clap_event_param_value_t *)h;
        paramEvent(v, offset, p->param_id, p->value);
        break;
    }
    default:
        break;
    }
}

/* Copies the assembled record into its slot and rings the channel's doorbell. */
static void publish(vl_plugin *v, const clap_process_t *process, int64_t callStart) {
    vstb_request *r = &v->request;
    const int64_t blockStart = (int64_t)v->k * v->B;
    double tempo = 120.0;
    double samplePos = (double)(blockStart - v->startFrame);
    uint32_t flags = 0;
    const clap_event_transport_t *t = process->transport;
    if (t) {
        if (t->flags & CLAP_TRANSPORT_HAS_TEMPO) tempo = t->tempo;
        if (t->flags & CLAP_TRANSPORT_IS_PLAYING) flags |= VSTB_REQ_PLAYING;
        if (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) {
            samplePos = (double)t->song_pos_seconds / (double)CLAP_SECTIME_FACTOR * v->sampleRate +
                        (double)(blockStart - callStart);
        }
    }
    r->blockIndex = v->k;
    r->frames = v->B;
    r->connectedMask = v->connectedThisBlock;
    r->flags = flags;
    r->tempo = tempo;
    r->samplePos = samplePos;
    r->ppqPos = samplePos / v->sampleRate * (tempo / 60.0);
    r->hostTimeMs = 0.0;
    memcpy(v->slots + (size_t)(v->k % VSTB_SLOTS) * VSTB_REQUEST_BYTES, r, sizeof *r);
    store32(&v->ctl->requestSeq, (int32_t)(v->k + 1));
    ring(&v->ctl->doorbell);
    memset(r, 0, offsetof(vstb_request, events) + r->eventCount * sizeof(vstb_event));
    v->connectedThisBlock = 0;
    v->k++;
}

/* Effect input for stream frames [frame, frame + count), from process offset `from`. */
static void writeInputs(vl_plugin *v, const clap_process_t *process, uint32_t from, int64_t frame, uint32_t count) {
    const uint32_t at = (uint32_t)(frame % VSTB_RING_FRAMES);
    for (uint32_t port = 0; port < g_desc.inPorts; port++) {
        const clap_audio_buffer_t *in = port < process->audio_inputs_count ? &process->audio_inputs[port] : NULL;
        const bool live = in && in->data32 && in->channel_count > 0 && in->constant_mask != ~(uint64_t)0;
        if (live) v->connectedThisBlock |= 1u << port;
        for (uint32_t c = 0; c < 2; c++) {
            float *ringBuf = v->inRings[port * 2 + c];
            if (live) {
                const float *src = in->data32[c < in->channel_count ? c : 0] + from;
                memcpy(ringBuf + at, src, count * sizeof(float));
            } else {
                memset(ringBuf + at, 0, count * sizeof(float));
            }
        }
    }
}

/* Output frames [t - L, t - L + n): the answered blocks, silence where none is. */
static void play(vl_plugin *v, const clap_process_t *process, int64_t t, uint32_t frames) {
    const uint32_t B = v->B;
    uint32_t f = 0;
    while (f < frames) {
        const int64_t x = t - (int64_t)v->L + f;
        uint32_t m;
        bool ready;
        int64_t j = -1;
        if (x < v->startFrame) {
            m = (uint32_t)((v->startFrame - x) < (int64_t)(frames - f) ? (v->startFrame - x) : (frames - f));
            ready = false;
        } else {
            j = x / B;
            int64_t left = (j + 1) * B - x;
            m = (uint32_t)(left < (int64_t)(frames - f) ? left : (frames - f));
            ready = load32(&v->ctl->doneBlock[j % VSTB_SLOTS]) == (int32_t)(j + 1);
            if (!ready && j != v->lastUnderrunBlock) {
                v->lastUnderrunBlock = j;
                v->underrunBlocks++;
                __atomic_add_fetch(&v->ctl->underruns, 1, __ATOMIC_SEQ_CST);
            }
        }
        const uint32_t at = ready ? (uint32_t)(x % VSTB_RING_FRAMES) : 0;
        for (uint32_t port = 0; port < process->audio_outputs_count; port++) {
            const clap_audio_buffer_t *out = &process->audio_outputs[port];
            if (!out->data32) continue;
            for (uint32_t c = 0; c < out->channel_count; c++) {
                float *dst = out->data32[c] + f;
                if (ready && port < g_desc.outPorts) memcpy(dst, v->outRings[port * 2 + (c < 2 ? c : 1)] + at, m * sizeof(float));
                else memset(dst, 0, m * sizeof(float));
            }
        }
        f += m;
    }
}

static void silence(const clap_process_t *process) {
    for (uint32_t port = 0; port < process->audio_outputs_count; port++) {
        const clap_audio_buffer_t *out = &process->audio_outputs[port];
        if (!out->data32) continue;
        for (uint32_t c = 0; c < out->channel_count; c++) memset(out->data32[c], 0, process->frames_count * sizeof(float));
    }
}

static clap_process_status vl_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    vl_plugin *v = self(plugin);
    const uint32_t frames = process->frames_count;
    if (frames == 0) return CLAP_PROCESS_CONTINUE;
    if (!attached(v)) {
        silence(process);
        return CLAP_PROCESS_CONTINUE;
    }
    const uint32_t B = v->B;
    const int64_t t = v->frame;
    const uint32_t at = (uint32_t)(t - (int64_t)v->k * B);

    /* Changes made outside process() and a reset's all-notes-off land at the call's start. */
    for (uint32_t i = 0; i < v->pendingCount; i++) paramEvent(v, at, v->pendingParams[i], v->pendingValues[i]);
    v->pendingCount = 0;
    if (v->releaseAll) {
        v->releaseAll = false;
        midiEvent(v, at, 0xb0, 123, 0);
    }
    if (v->buzzStopPending) {
        v->buzzStopPending = false;
        addEvent(v, at, VSTB_EV_BUZZ_STOP, 0, 0.0f, 0, 0, 0);
    }

    const clap_input_events_t *events = process->in_events;
    const uint32_t count = events ? events->size(events) : 0;
    uint32_t next = 0;
    for (int64_t a = t; a < t + frames;) {
        const int64_t blockEnd = (int64_t)(v->k + 1) * B;
        const int64_t b = (t + frames) < blockEnd ? (t + frames) : blockEnd;
        while (next < count) {
            const clap_event_header_t *h = events->get(events, next);
            uint32_t time = h->time < frames ? h->time : frames - 1;
            if (t + time >= b) break;
            applyEvent(v, h, (uint32_t)(t + time - (int64_t)v->k * B));
            next++;
        }
        if (g_desc.inPorts) writeInputs(v, process, (uint32_t)(a - t), a, (uint32_t)(b - a));
        if (b == blockEnd) publish(v, process, t);
        a = b;
    }
    v->frame = t + frames;
    play(v, process, t, frames);
    /* A fresh state after parameter changes, at most every STATE_REFRESH_BLOCKS. */
    if (v->stateDirty && !v->callbackPending && v->helloSent && v->k - v->lastStateRequestBlock >= STATE_REFRESH_BLOCKS) {
        v->callbackPending = true;
        v->host->request_callback(v->host);
    }
    return CLAP_PROCESS_CONTINUE;
}


/* ---- prometheos.buzz-machine/1 -------------------------------------------------- */

typedef struct {
    const char *(*layout)(const clap_plugin_t *);
    void (*set_master_info)(const clap_plugin_t *, int32_t, int32_t, double);
    void (*set_num_tracks)(const clap_plugin_t *, int32_t);
    void (*set_attribute)(const clap_plugin_t *, int32_t, int32_t);
    void (*set_value)(const clap_plugin_t *, int32_t, int32_t, int32_t);
    void (*tick)(const clap_plugin_t *, uint32_t);
    void (*stop)(const clap_plugin_t *);
} prometheos_buzz_machine_t;

static bool isBuzz(void) { return !strcmp(g_desc.format, "buzz"); }
static const char *buzz_layout(const clap_plugin_t *plugin) { (void)plugin; return g_desc.buzzLayout; }
static void buzz_master(const clap_plugin_t *plugin, int32_t bpm, int32_t tpb, double spt) {
    vl_plugin *v = self(plugin); v->buzzBpm = bpm; v->buzzTpb = tpb; v->buzzSpt = spt; v->buzzMasterDirty = true;
}
static void buzz_tracks(const clap_plugin_t *plugin, int32_t tracks) {
    vl_plugin *v = self(plugin); v->buzzTracks = tracks; v->buzzTracksDirty = true;
}
static void buzz_attribute(const clap_plugin_t *plugin, int32_t index, int32_t value) {
    vl_plugin *v = self(plugin);
    if (index < 0 || index > 65535 || v->buzzAttrCount >= MAX_BUZZ_PENDING) return;
    v->buzzAttrIndex[v->buzzAttrCount] = (uint16_t)index;
    v->buzzAttrValue[v->buzzAttrCount++] = value;
}
static void buzz_value(const clap_plugin_t *plugin, int32_t track, int32_t index, int32_t value) {
    vl_plugin *v = self(plugin);
    if (track < -1 || track > 254 || index < 0 || index > 65535 || v->buzzValueCount >= MAX_BUZZ_PENDING) return;
    v->buzzValues[v->buzzValueCount].track = (int16_t)track;
    v->buzzValues[v->buzzValueCount].index = (uint16_t)index;
    v->buzzValues[v->buzzValueCount].value = value;
    v->buzzValueCount++;
}
static void buzz_tick(const clap_plugin_t *plugin, uint32_t sampleOffset) {
    vl_plugin *v = self(plugin);
    uint32_t offset = (uint32_t)(v->frame - (int64_t)v->k * v->B) + sampleOffset;
    if (offset >= v->B) offset = v->B - 1;
    if (v->buzzMasterDirty) {
        addEvent(v, offset, VSTB_EV_BUZZ_MASTER, (uint16_t)(v->buzzTpb > 0 ? v->buzzTpb : 4),
                 (float)(v->buzzSpt > 0 ? v->buzzSpt : 1.0), 0, 0, 0);
        v->buzzMasterDirty = false;
    }
    if (v->buzzTracksDirty) {
        addEvent(v, offset, VSTB_EV_BUZZ_TRACKS, 0, (float)v->buzzTracks, 0, 0, 0);
        v->buzzTracksDirty = false;
    }
    for (uint32_t i = 0; i < v->buzzAttrCount; ++i)
        addEvent(v, offset, VSTB_EV_BUZZ_ATTR, v->buzzAttrIndex[i], (float)v->buzzAttrValue[i], 0, 0, 0);
    v->buzzAttrCount = 0;
    for (uint32_t i = 0; i < v->buzzValueCount; ++i) {
        uint16_t encoded = (uint16_t)(v->buzzValues[i].track + 1);
        addEvent(v, offset, VSTB_EV_BUZZ_VALUE, v->buzzValues[i].index, (float)v->buzzValues[i].value,
                 (uint8_t)(encoded & 255), (uint8_t)(encoded >> 8), 0);
    }
    v->buzzValueCount = 0;
    addEvent(v, offset, VSTB_EV_BUZZ_TICK, 0, 0.0f, 0, 0, 0);
    v->stateDirty = true;
}
static void buzz_stop(const clap_plugin_t *plugin) { self(plugin)->buzzStopPending = true; }
static const prometheos_buzz_machine_t g_buzz_machine = {
    buzz_layout, buzz_master, buzz_tracks, buzz_attribute, buzz_value, buzz_tick, buzz_stop
};

/* ---- lifecycle ------------------------------------------------------------------ */

static bool vl_init(const clap_plugin_t *plugin) {
    vl_plugin *v = self(plugin);
    v->hostRuntime = v->host->get_extension(v->host, PROMETHEOS_EXT_RUNTIME);
    v->hostLog = v->host->get_extension(v->host, CLAP_EXT_LOG);
    v->channel = -1;
    for (int i = 0; i < (int)VSTB_MAX_CHANNELS; i++) {
        if (!g_channelUsed[i]) {
            g_channelUsed[i] = true;
            v->channel = i;
            break;
        }
    }
    if (v->channel < 0) {
        logf_(v, CLAP_LOG_ERROR, "vstloader: too many instances in one module");
        return false;
    }
    uint8_t *base = g_channels[v->channel];
    memset(base, 0, VSTB_CHANNEL_BYTES);
    v->ctl = (vstb_channel_ctl *)base;
    v->slots = base + VSTB_SLOTS_OFFSET;
    for (uint32_t i = 0; i < MAX_PORTS * 2; i++) {
        v->inRings[i] = (float *)(base + VSTB_IN_RINGS_OFFSET + i * VSTB_RING_FRAMES * 4u);
        v->outRings[i] = (float *)(base + VSTB_OUT_RINGS_OFFSET + i * VSTB_RING_FRAMES * 4u);
    }
    v->paramValues = calloc(g_desc.paramCount ? g_desc.paramCount : 1, sizeof(double));
    for (uint32_t i = 0; i < g_desc.paramCount; i++) v->paramValues[i] = g_desc.params[i].defaultValue;
    return v->paramValues != NULL;
}

static void vl_destroy(const clap_plugin_t *plugin) {
    vl_plugin *v = self(plugin);
    if (v->helloSent) sendFrame(v, VL_BYE, NULL, 0, NULL, 0);
    if (v->channel >= 0) {
        store32(&v->ctl->state, VSTB_STATE_CLOSED);
        g_channelUsed[v->channel] = false;
    }
    free(v->paramValues);
    free(v->state);
    free(v);
}

static bool vl_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void)min_frames;
    (void)max_frames;
    vl_plugin *v = self(plugin);
    v->sampleRate = sample_rate;
    v->active = true;
    sendHello(v);
    return true;
}

static void vl_deactivate(const clap_plugin_t *plugin) { self(plugin)->active = false; }
static bool vl_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void vl_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }
static void vl_reset(const clap_plugin_t *plugin) { self(plugin)->releaseAll = true; }
static void vl_on_main_thread(const clap_plugin_t *plugin) {
    vl_plugin *v = self(plugin);
    v->callbackPending = false;
    sendHello(v);
    if (v->stateDirty && v->helloSent) {
        v->stateDirty = false;
        v->lastStateRequestBlock = v->k;
        sendFrame(v, VL_GET_STATE, NULL, 0, NULL, 0);
    }
}

/* ---- extensions ----------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input ? g_desc.inPorts : g_desc.outPorts;
}

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void)plugin;
    if (index >= (is_input ? g_desc.inPorts : g_desc.outPorts)) return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    if (index == 0) copyField(info->name, sizeof info->name, is_input ? "Input" : "Output");
    else snprintf(info->name, sizeof info->name, "%s %u", is_input ? "Input" : "Output", index + 1);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = {ports_count, ports_get};

static uint32_t notes_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input && g_desc.synth ? 1 : 0;
}

static bool notes_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info) {
    (void)plugin;
    if (!is_input || !g_desc.synth || index != 0) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    copyField(info->name, sizeof info->name, "MIDI");
    return true;
}

static const clap_plugin_note_ports_t g_note_ports = {notes_count, notes_get};

static uint32_t latency_get(const clap_plugin_t *plugin) {
    (void)plugin;
    return g_desc.bridgeLatency + g_desc.pluginLatency;
}

static const clap_plugin_latency_t g_latency = {latency_get};

static uint32_t params_count(const clap_plugin_t *plugin) {
    (void)plugin;
    return g_desc.paramCount;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void)plugin;
    if (index >= g_desc.paramCount) return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    copyField(info->name, sizeof info->name, g_desc.params[index].name);
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = g_desc.params[index].defaultValue;
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    vl_plugin *v = self(plugin);
    if (id >= g_desc.paramCount) return false;
    *value = v->paramValues[id];
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size) {
    (void)plugin;
    if (id >= g_desc.paramCount || size == 0) return false;
    const char *label = g_desc.params[id].label;
    snprintf(out, size, label[0] ? "%.3f %s" : "%.3f", value, label);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    (void)plugin;
    if (id >= g_desc.paramCount) return false;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (end == text) return false;
    *value = parsed < 0.0 ? 0.0 : parsed > 1.0 ? 1.0 : parsed;
    return true;
}

/* Changes outside process() go out at the start of the next call. */
static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    (void)out;
    vl_plugin *v = self(plugin);
    const uint32_t count = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < count; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
        const clap_event_param_value_t *p = (const clap_event_param_value_t *)h;
        if (p->param_id >= g_desc.paramCount || v->pendingCount >= MAX_PARAMS) continue;
        v->paramValues[p->param_id] = p->value;
        v->pendingParams[v->pendingCount] = p->param_id;
        v->pendingValues[v->pendingCount] = (float)p->value;
        v->pendingCount++;
    }
}

static const clap_plugin_params_t g_params = {params_count, params_get_info, params_get_value,
                                              params_value_to_text, params_text_to_value, params_flush};

/* State is the plugin's chunk (or its parameters), as the runtime last reported it. */
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    vl_plugin *v = self(plugin);
    uint32_t done = 0;
    while (done < v->stateSize) {
        int64_t n = stream->write(stream, v->state + done, v->stateSize - done);
        if (n <= 0) return false;
        done += (uint32_t)n;
    }
    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    vl_plugin *v = self(plugin);
    uint32_t cap = 65536, size = 0;
    uint8_t *data = malloc(cap);
    if (!data) return false;
    for (;;) {
        if (size == cap) {
            uint8_t *bigger = realloc(data, cap *= 2);
            if (!bigger) {
                free(data);
                return false;
            }
            data = bigger;
        }
        int64_t n = stream->read(stream, data + size, cap - size);
        if (n < 0) {
            free(data);
            return false;
        }
        if (n == 0) break;
        size += (uint32_t)n;
    }
    free(v->state);
    v->state = data;
    v->stateSize = size;
    v->stateToSend = !(v->helloSent && sendFrame(v, VL_SET_STATE, data, size, NULL, 0));
    return true;
}

static const clap_plugin_state_t g_state = {state_save, state_load};

static int32_t runtime_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    (void)plugin;
    const int32_t length = (int32_t)strlen(g_desc.runtime);
    if (length == 0) return -1;
    if (capacity > 0) copyField(uri, capacity, g_desc.runtime);
    return length;
}

static bool runtime_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    vl_plugin *v = self(plugin);
    if (size < 4) return false;
    uint32_t op;
    memcpy(&op, buffer, 4);
    const uint8_t *body = (const uint8_t *)buffer + 4;
    const uint32_t bodySize = size - 4;
    switch (op) {
    case VL_STATE: {
        uint8_t *copy = bodySize ? malloc(bodySize) : NULL;
        if (bodySize && !copy) return false;
        if (bodySize) memcpy(copy, body, bodySize);
        free(v->state);
        v->state = copy;
        v->stateSize = bodySize;
        return true;
    }
    case VL_ERROR: {
        char message[512];
        uint32_t n = bodySize < sizeof message - 12 ? bodySize : (uint32_t)sizeof message - 12;
        memcpy(message, "vstloader: ", 11);
        memcpy(message + 11, body, n);
        message[11 + n] = 0;
        logf_(v, CLAP_LOG_ERROR, message);
        return true;
    }
    case VL_RESEND:
        v->helloSent = false;
        v->stateToSend = v->state != NULL;
        sendHello(v);
        return true;
    default:
        return false;
    }
}

static const prometheos_plugin_runtime_t g_runtime = {runtime_get_uri, runtime_receive};

static const void *vl_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &g_audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &g_note_ports;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &g_latency;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &g_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &g_state;
    if (!strcmp(id, PROMETHEOS_EXT_RUNTIME)) return &g_runtime;
    if (isBuzz() && !strcmp(id, PROMETHEOS_BUZZ_MACHINE_EXT)) return &g_buzz_machine;
    return NULL;
}

/* ---- factory and entry ------------------------------------------------------------ */

static const clap_plugin_t *create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id) {
    (void)factory;
    if (strcmp(id, g_desc.id) != 0) return NULL;
    vl_plugin *v = calloc(1, sizeof(vl_plugin));
    if (!v) return NULL;
    v->host = host;
    v->plugin.desc = &g_clap_desc;
    v->plugin.plugin_data = v;
    v->plugin.init = vl_init;
    v->plugin.destroy = vl_destroy;
    v->plugin.activate = vl_activate;
    v->plugin.deactivate = vl_deactivate;
    v->plugin.start_processing = vl_start_processing;
    v->plugin.stop_processing = vl_stop_processing;
    v->plugin.reset = vl_reset;
    v->plugin.process = vl_process;
    v->plugin.get_extension = vl_get_extension;
    v->plugin.on_main_thread = vl_on_main_thread;
    return &v->plugin;
}

static uint32_t get_plugin_count(const clap_plugin_factory_t *factory) {
    (void)factory;
    return g_desc.id[0] ? 1 : 0;
}

static const clap_plugin_descriptor_t *get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void)factory;
    return index == 0 && g_desc.id[0] ? &g_clap_desc : NULL;
}

static const clap_plugin_factory_t g_factory = {get_plugin_count, get_plugin_descriptor, create_plugin};

static bool entry_init(const char *plugin_path) {
    copyField(g_bundle, sizeof g_bundle, plugin_path);
    if (!readDescriptor(plugin_path)) return false;
    if (!loadDependencies(plugin_path)) return false;
    g_clap_desc.clap_version = (clap_version_t)CLAP_VERSION_INIT;
    g_clap_desc.id = g_desc.id;
    g_clap_desc.name = g_desc.name;
    g_clap_desc.vendor = g_desc.vendor;
    g_clap_desc.url = "";
    g_clap_desc.manual_url = "";
    g_clap_desc.support_url = "";
    g_clap_desc.version = g_desc.version;
    g_clap_desc.description = isBuzz() ? "Native Jeskola Buzz machine, run by Boxedwine" : "Windows VST plugin, run by Boxedwine";
    g_clap_desc.features = g_desc.synth ? g_features_synth : g_features_effect;
    return true;
}

static void entry_deinit(void) {
    for (uint32_t i = 0; i < g_desc.dependencyCount; ++i) {
        free(g_desc.dependencies[i].bytes);
        g_desc.dependencies[i].bytes = NULL;
        g_desc.dependencies[i].size = 0;
    }
    free(g_desc.params);
    g_desc.params = NULL;
}

static const void *entry_get_factory(const char *factory_id) {
    return !strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
