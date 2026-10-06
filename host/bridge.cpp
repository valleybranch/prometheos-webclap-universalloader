// vsthost --bridge: the Wine side of real-time hosting through Boxedwine's
// /dev/vstbridge (layout and protocol: include/vstbridge_abi.h).
//
// The main thread serves the control channel: requests from the page (PING,
// LOAD, DESCRIBE, GET_STATE, SET_STATE, UNLOAD) arrive as messages and get one
// response each. LOAD starts one thread per plugin instance (yabridge's
// "plugin groups": one Wine process, many instances). That thread loads the
// plugin, warms it up, binds its own handle to the audio channel and then
// loops: ReadFile (blocks until the AudioWorklet publishes the next request),
// process one block, WriteFile the outputs. It has no timing of its own.
// Commands for an instance (state, describe, unload) run on its own thread:
// the control thread queues them and kicks the channel, which makes the
// instance's next read return a kick record instead of audio.
#include "bridge.h"

#include "json.h"
#include "pe_imports.h"
#include "plugin_instance.h"
#include "vstbridge_abi.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

const char *kDevice = "Z:\\dev\\vstbridge";

double nowMs() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

void say(const char *fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    std::printf("vsthost: %s\n", line);
    std::fflush(stdout);
}

// Device buffers must be page-aligned (see vstbridge_abi.h).
uint8_t *pageAlloc(size_t bytes) {
    return static_cast<uint8_t *>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

HANDLE openDevice() {
    return CreateFileA(kDevice, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

bool writeAll(HANDLE h, const void *data, DWORD size) {
    DWORD n = 0;
    return WriteFile(h, data, size, &n, nullptr) && n == size;
}

// One record, as the device delivers it. Returns its size, or -1 on error.
// (The single-threaded emulator build can't block, so nothing pending reads 0.)
long readRecord(HANDLE h, void *buffer, DWORD size) {
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, buffer, size, &n, nullptr)) return -1;
        if (n) return static_cast<long>(n);
        Sleep(1);
    }
}

struct Instance {
    int channel = 0;
    std::string path;
    double rate = 48000;
    int block = 256;
    bool warmup = true;
    std::unique_ptr<PluginInstance> plugin;
    HANDLE thread = nullptr;
    HANDLE loaded = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE commandDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::string error;
    std::string describe;
    double loadMs = 0;
    // The pending command, set by the control thread before it kicks.
    uint32_t op = 0;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> reply;
    int status = VSTB_STATUS_OK;
    bool quit = false;
};

std::unique_ptr<Instance> gInstances[VSTB_MAX_CHANNELS + 1];

void runCommand(Instance &in) {
    in.reply.clear();
    in.status = VSTB_STATUS_OK;
    std::string error;
    switch (in.op) {
    case VSTB_OP_DESCRIBE: {
        const std::string json = in.plugin->describeJson();
        in.reply.assign(json.begin(), json.end());
        break;
    }
    case VSTB_OP_GET_STATE: in.reply = in.plugin->getState(); break;
    case VSTB_OP_SET_STATE:
        if (!in.plugin->setState(in.payload.data(), in.payload.size(), error)) {
            in.status = VSTB_STATUS_ERROR;
            in.reply.assign(error.begin(), error.end());
        }
        break;
    case VSTB_OP_UNLOAD: in.quit = true; break;
    default: in.status = VSTB_STATUS_ERROR; break;
    }
}

DWORD WINAPI instanceMain(void *arg) {
    Instance &in = *static_cast<Instance *>(arg);
    prepareAudioThread();
    const double t0 = nowMs();
    in.plugin = loadPlugin(in.path, in.rate, in.block, in.error);
    if (!in.plugin) {
        SetEvent(in.loaded);
        return 1;
    }
    if (in.warmup) in.plugin->warmUp();
    in.loadMs = nowMs() - t0;
    in.describe = in.plugin->describeJson();
    HANDLE device = openDevice();
    if (device == INVALID_HANDLE_VALUE) {
        in.error = "cannot open Z:\\dev\\vstbridge, error " + std::to_string(GetLastError());
        in.plugin->close();
        SetEvent(in.loaded);
        return 1;
    }
    vstb_attach attach{};
    attach.magic = VSTB_ATTACH_MAGIC;
    attach.channel = static_cast<uint32_t>(in.channel);
    attach.sampleRate = static_cast<uint32_t>(in.rate);
    attach.blockFrames = static_cast<uint32_t>(in.block);
    attach.inPorts = static_cast<uint32_t>(in.plugin->inPorts());
    attach.outPorts = static_cast<uint32_t>(in.plugin->outPorts());
    attach.pluginLatency = static_cast<uint32_t>(in.plugin->latency());
    writeAll(device, &attach, sizeof attach);
    SetEvent(in.loaded);

    const DWORD inBytes = in.plugin->inPorts() * 2 * in.block * sizeof(float);
    const DWORD outBytes = in.plugin->outPorts() * 2 * in.block * sizeof(float);
    uint8_t *request = pageAlloc(VSTB_REQUEST_BYTES + inBytes);
    uint8_t *response = pageAlloc(VSTB_RESPONSE_HEADER_BYTES + outBytes);
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    while (!in.quit) {
        const long n = readRecord(device, request, VSTB_REQUEST_BYTES + inBytes);
        if (n < 0) break;
        const vstb_request &r = *reinterpret_cast<const vstb_request *>(request);
        if (r.flags & VSTB_REQ_KICK) {
            runCommand(in);
            SetEvent(in.commandDone);
            continue;
        }
        if (n != static_cast<long>(VSTB_REQUEST_BYTES + inBytes)) continue;
        QueryPerformanceCounter(&a);
        in.plugin->process(r, reinterpret_cast<const float *>(request + VSTB_REQUEST_BYTES),
                          reinterpret_cast<float *>(response + VSTB_RESPONSE_HEADER_BYTES));
        QueryPerformanceCounter(&b);
        auto &h = *reinterpret_cast<vstb_response *>(response);
        h.blockIndex = r.blockIndex;
        h.frames = static_cast<uint32_t>(in.block);
        h.processUs = static_cast<uint32_t>((b.QuadPart - a.QuadPart) * 1000000 / freq.QuadPart);
        h.status = in.plugin->takeIoChanged() ? VSTB_RESP_IO_CHANGED : 0;
        writeAll(device, response, VSTB_RESPONSE_HEADER_BYTES + outBytes);
    }
    in.plugin->close();
    CloseHandle(device);
    VirtualFree(request, 0, MEM_RELEASE);
    VirtualFree(response, 0, MEM_RELEASE);
    return 0;
}

std::string field(const std::string &text, const char *key) {
    const std::string k = std::string(key) + "=";
    size_t p = text.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    if (std::string(key) == "path") return text.substr(p); // the path runs to the end, spaces included
    return text.substr(p, text.find(' ', p) - p);
}

class Control {
  public:
    bool open() {
        handle_ = openDevice();
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        header_ = pageAlloc(4096);
        payload_ = pageAlloc(VSTB_CTRL_BYTES);
        out_ = pageAlloc(VSTB_CTRL_BYTES);
        vstb_attach attach{};
        attach.magic = VSTB_ATTACH_MAGIC;
        attach.channel = 0;
        return writeAll(handle_, &attach, sizeof attach);
    }

    void serve() {
        for (;;) {
            if (readRecord(handle_, header_, sizeof(vstb_msg)) != static_cast<long>(sizeof(vstb_msg))) continue;
            const vstb_msg msg = *reinterpret_cast<const vstb_msg *>(header_);
            std::string text;
            if (msg.length) {
                if (readRecord(handle_, payload_, msg.length) != static_cast<long>(msg.length)) continue;
                text.assign(reinterpret_cast<const char *>(payload_), msg.length);
            }
            if (msg.op == VSTB_OP_PUT_FILE) putFile(msg);
            else handle(msg, text);
        }
    }

  private:
    void respond(const vstb_msg &request, int status, const void *data, size_t size) {
        size = std::min<size_t>(size, VSTB_CTRL_BYTES - sizeof(vstb_msg));
        vstb_msg &msg = *reinterpret_cast<vstb_msg *>(out_);
        msg = vstb_msg{};
        msg.seq = request.seq;
        msg.op = request.op;
        msg.channel = request.channel;
        msg.length = static_cast<uint32_t>(size);
        msg.status = status;
        if (size) std::memcpy(out_ + sizeof(vstb_msg), data, size);
        writeAll(handle_, out_, static_cast<DWORD>(sizeof(vstb_msg) + size));
    }
    void respond(const vstb_msg &request, int status, const std::string &text) {
        respond(request, status, text.data(), text.size());
    }

    // Writes one piece of a guest file: u32 offset, u32 total, u32 pathBytes, path, bytes.
    void putFile(const vstb_msg &msg) {
        uint32_t head[3] = {};
        if (msg.length < sizeof head) return respond(msg, VSTB_STATUS_ERROR, "short PUT_FILE");
        std::memcpy(head, payload_, sizeof head);
        const uint32_t offset = head[0], total = head[1], pathBytes = head[2];
        if (sizeof head + pathBytes > msg.length) return respond(msg, VSTB_STATUS_ERROR, "bad PUT_FILE path");
        const std::string path(reinterpret_cast<const char *>(payload_) + sizeof head, pathBytes);
        const uint8_t *data = payload_ + sizeof head + pathBytes;
        const size_t size = msg.length - sizeof head - pathBytes;
        for (size_t i = 3; i < path.size(); ++i)
            if (path[i] == '\\' || path[i] == '/') CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
        FILE *f = std::fopen(path.c_str(), offset == 0 ? "wb" : "r+b");
        if (!f) return respond(msg, VSTB_STATUS_ERROR, "cannot open " + path);
        std::fseek(f, static_cast<long>(offset), SEEK_SET);
        const size_t written = std::fwrite(data, 1, size, f);
        std::fclose(f);
        if (written != size) return respond(msg, VSTB_STATUS_ERROR, "short write to " + path);
        if (offset + size == total) say("wrote %s (%u bytes)", path.c_str(), total);
        respond(msg, VSTB_STATUS_OK, "");
    }

    // Runs a command on the instance's own thread.
    bool onInstance(Instance &in, const vstb_msg &msg, const std::string &payload) {
        in.op = msg.op;
        in.payload.assign(payload.begin(), payload.end());
        vstb_msg kick{};
        kick.op = VSTB_OP_KICK;
        kick.channel = static_cast<uint32_t>(in.channel);
        writeAll(handle_, &kick, sizeof kick);
        return WaitForSingleObject(in.commandDone, 120000) == WAIT_OBJECT_0;
    }

    void handle(const vstb_msg &msg, const std::string &payload) {
        const int ch = static_cast<int>(msg.channel);
        const bool validChannel = ch >= 1 && ch <= static_cast<int>(VSTB_MAX_CHANNELS);
        switch (msg.op) {
        case VSTB_OP_PING: respond(msg, VSTB_STATUS_OK, "pong"); return;
        case VSTB_OP_PROBE_IMPORTS: {
            ImportProbe probe;
            std::string error;
            if (!probePeImports(payload, probe, error)) return respond(msg, VSTB_STATUS_ERROR, error);
            return respond(msg, VSTB_STATUS_OK, importProbeJson(probe));
        }
        case VSTB_OP_LOAD: {
            if (!validChannel) return respond(msg, VSTB_STATUS_ERROR, "bad channel");
            if (gInstances[ch]) return respond(msg, VSTB_STATUS_ERROR, "channel in use");
            auto in = std::make_unique<Instance>();
            in->channel = ch;
            in->path = field(payload, "path");
            in->rate = std::atof(field(payload, "rate").c_str());
            in->block = std::atoi(field(payload, "block").c_str());
            in->warmup = field(payload, "warmup") != "0";
            if (in->rate <= 0) in->rate = 48000;
            if (in->block < static_cast<int>(VSTB_MIN_BLOCK_FRAMES) || in->block > static_cast<int>(VSTB_MAX_BLOCK_FRAMES) ||
                (in->block & (in->block - 1)))
                return respond(msg, VSTB_STATUS_ERROR, "block must be a power of two in 32..1024");
            say("channel %d: loading %s (%.0f Hz, %d frames)", ch, in->path.c_str(), in->rate, in->block);
            in->thread = CreateThread(nullptr, 1 << 20, instanceMain, in.get(), 0, nullptr);
            WaitForSingleObject(in->loaded, INFINITE);
            if (!in->error.empty()) {
                say("channel %d: %s", ch, in->error.c_str());
                WaitForSingleObject(in->thread, 30000);
                return respond(msg, VSTB_STATUS_ERROR, in->error);
            }
            say("channel %d: ready after %.0f ms (%s)", ch, in->loadMs, in->path.c_str());
            const std::string describe = in->describe;
            gInstances[ch] = std::move(in);
            return respond(msg, VSTB_STATUS_OK, describe);
        }
        case VSTB_OP_DESCRIBE:
        case VSTB_OP_GET_STATE:
        case VSTB_OP_SET_STATE:
        case VSTB_OP_UNLOAD: {
            if (!validChannel || !gInstances[ch]) return respond(msg, VSTB_STATUS_ERROR, "no instance on that channel");
            Instance &in = *gInstances[ch];
            if (!onInstance(in, msg, payload)) return respond(msg, VSTB_STATUS_ERROR, "instance did not answer");
            const std::vector<uint8_t> reply = in.reply;
            const int status = in.status;
            if (msg.op == VSTB_OP_UNLOAD) {
                WaitForSingleObject(in.thread, 60000);
                gInstances[ch].reset();
                say("channel %d: unloaded", ch);
            }
            return respond(msg, status, reply.data(), reply.size());
        }
        default: respond(msg, VSTB_STATUS_ERROR, "unknown op");
        }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    uint8_t *header_ = nullptr;
    uint8_t *payload_ = nullptr;
    uint8_t *out_ = nullptr;
};

// ---- offline replay ----------------------------------------------------------------

struct ReplayHeader {
    char magic[4]; // "VSRP"
    uint32_t version;
    uint32_t sampleRate;
    uint32_t blockFrames;
    uint32_t inPorts;
    uint32_t outPorts;
    uint32_t blocks;
    uint32_t warmup;
};

struct Replay {
    std::string plugin, capture, out;
    int status = 1;
    std::string error;
    double processMs = 0, loadMs = 0;
    uint32_t blocks = 0;
    std::string slowBlocks; // "[block, ms]" pairs for blocks that took 8 ms or more (first 64)
    ReplayHeader header{};
};

std::vector<uint8_t> readFile(const std::string &path) {
    std::vector<uint8_t> data;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
    }
    return data;
}

DWORD WINAPI replayMain(void *arg) {
    Replay &job = *static_cast<Replay *>(arg);
    prepareAudioThread();
    const std::vector<uint8_t> capture = readFile(job.capture);
    if (capture.size() < sizeof(ReplayHeader) || std::memcmp(capture.data(), "VSRP", 4)) {
        job.error = "cannot read capture " + job.capture;
        return 1;
    }
    std::memcpy(&job.header, capture.data(), sizeof job.header);
    const ReplayHeader &h = job.header;
    const double t0 = nowMs();
    std::unique_ptr<PluginInstance> loaded = loadPlugin(job.plugin, h.sampleRate, static_cast<int>(h.blockFrames), job.error);
    if (!loaded) return 1;
    PluginInstance &plugin = *loaded;
    if (h.warmup) plugin.warmUp();
    job.loadMs = nowMs() - t0;
    if (static_cast<uint32_t>(plugin.inPorts()) != h.inPorts || static_cast<uint32_t>(plugin.outPorts()) != h.outPorts) {
        job.error = "capture ports " + std::to_string(h.inPorts) + "/" + std::to_string(h.outPorts) +
                    " differ from the plugin's " + std::to_string(plugin.inPorts()) + "/" + std::to_string(plugin.outPorts());
        return 1;
    }
    const size_t inBytes = h.inPorts * 2 * h.blockFrames * sizeof(float);
    const size_t outFloats = h.outPorts * 2 * h.blockFrames;
    const size_t record = VSTB_REQUEST_BYTES + inBytes;
    const uint32_t blocks = std::min<uint32_t>(h.blocks, (capture.size() - sizeof h) / record);
    std::vector<float> out(static_cast<size_t>(blocks) * outFloats);
    const double t1 = nowMs();
    int slow = 0;
    for (uint32_t k = 0; k < blocks; ++k) {
        const uint8_t *rec = capture.data() + sizeof h + k * record;
        const double b0 = nowMs();
        plugin.process(*reinterpret_cast<const vstb_request *>(rec), reinterpret_cast<const float *>(rec + VSTB_REQUEST_BYTES),
                       out.data() + k * outFloats);
        const double ms = nowMs() - b0;
        if (ms >= 8.0 && slow++ < 64) {
            job.slowBlocks += (job.slowBlocks.empty() ? "[" : ",[") + std::to_string(k) + "," + std::to_string(static_cast<int>(ms)) + "]";
        }
    }
    job.processMs = nowMs() - t1;
    job.blocks = blocks;
    plugin.close();
    for (size_t i = 3; i < job.out.size(); ++i)
        if (job.out[i] == '\\' || job.out[i] == '/') CreateDirectoryA(job.out.substr(0, i).c_str(), nullptr);
    if (FILE *f = std::fopen(job.out.c_str(), "wb")) {
        std::fwrite(out.data(), sizeof(float), out.size(), f);
        std::fclose(f);
    } else {
        job.error = "cannot write " + job.out;
        return 1;
    }
    job.status = 0;
    return 0;
}

} // namespace

int runBridge() {
    Control control;
    if (!control.open()) {
        say("cannot open %s (error %lu): does this Boxedwine build have the vstbridge patch?", kDevice, GetLastError());
        return 1;
    }
    say("bridge serving %s", kDevice);
    control.serve();
    return 0;
}

int runReplay(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: vsthost --replay <plugin.dll> <capture.bin> <out.f32>\n");
        return 2;
    }
    Replay job;
    job.plugin = argv[2];
    job.capture = argv[3];
    job.out = argv[4];
    HANDLE thread = CreateThread(nullptr, 1 << 20, replayMain, &job, 0, nullptr);
    WaitForSingleObject(thread, INFINITE);
    const double seconds = job.header.sampleRate ? double(job.blocks) * job.header.blockFrames / job.header.sampleRate : 0;
    char report[2048];
    std::snprintf(report, sizeof report,
                  "{\"ok\":%s,\"error\":\"%s\",\"blocks\":%u,\"blockFrames\":%u,\"sampleRate\":%u,\"outPorts\":%u,"
                  "\"loadMs\":%.1f,\"processMs\":%.1f,\"realtimeFactor\":%.3f,\"slowBlocks\":[%s]}\n",
                  job.status ? "false" : "true", jsonEscape(job.error).c_str(), job.blocks, job.header.blockFrames,
                  job.header.sampleRate, job.header.outPorts, job.loadMs, job.processMs,
                  job.processMs > 0 ? seconds * 1000.0 / job.processMs : 0.0, job.slowBlocks.c_str());
    if (FILE *f = std::fopen((job.out + ".json").c_str(), "wb")) {
        std::fputs(report, f);
        std::fclose(f);
    }
    // The console wraps long lines, so the report goes to <out>.json and this is the marker.
    std::printf("vsthost: replay %s\n", job.status ? "failed" : "done");
    std::fflush(stdout);
    return job.status;
}
