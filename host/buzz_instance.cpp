#include "buzz_instance.h"
#include "plugin_instance.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <sstream>

namespace {
template <typename R, typename... A>
static R machineCall(CMachineInterface *m, unsigned slot, A... args) {
    using Fn = R (__attribute__((thiscall)) *)(CMachineInterface *, A...);
    return reinterpret_cast<Fn>(m->vtable[slot])(m, args...);
}
static void machineDestroy(CMachineInterface *m) {
    if (!m) return;
    using Fn = void (__attribute__((thiscall)) *)(CMachineInterface *, unsigned);
    reinterpret_cast<Fn>(m->vtable[0])(m, 1u);
}

static std::string jsonString(const char *text) {
    return "\"" + jsonEscape(text ? std::string(text) : std::string()) + "\"";
}

class MemoryInput final : public CMachineDataInput {
public:
    MemoryInput(const uint8_t *data, size_t size) : p_(data), left_(size) {}
    void Read(void *dst, int const bytes) override {
        const size_t n = std::min(left_, static_cast<size_t>(std::max(0, bytes)));
        if (n) std::memcpy(dst, p_, n);
        if (n < static_cast<size_t>(bytes)) std::memset(static_cast<uint8_t *>(dst) + n, 0, bytes - n);
        p_ += n;
        left_ -= n;
    }
private:
    const uint8_t *p_;
    size_t left_;
};

class MemoryOutput final : public CMachineDataOutput {
public:
    explicit MemoryOutput(std::vector<uint8_t> &out) : out_(out) {}
    void Write(void *src, int const bytes) override {
        if (bytes <= 0) return;
        auto *p = static_cast<const uint8_t *>(src);
        out_.insert(out_.end(), p, p + bytes);
    }
private:
    std::vector<uint8_t> &out_;
};
}

BuzzMachineInstance::~BuzzMachineInstance() { close(); }

void BuzzMachineInstance::BuzzCallbacks::ClearAuxBuffer() { std::memset(aux, 0, sizeof aux); }
void BuzzMachineInstance::BuzzCallbacks::MessageBox(char const *text) { lastMessage = text ? text : ""; }

int BuzzMachineInstance::paramSize(const CMachineParameter &p) { return p.Type == pt_word ? 2 : 1; }

bool BuzzMachineInstance::create(std::string &error) {
    std::fprintf(stderr, "[buzz] CreateMachine begin\n"); std::fflush(stderr);
    machine_ = createMachine_ ? createMachine_() : nullptr;
    std::fprintf(stderr, "[buzz] CreateMachine end %p\n", static_cast<void *>(machine_)); std::fflush(stderr);
    if (!machine_) {
        error = "Buzz CreateMachine returned null";
        return false;
    }
    machine_->pCB = &callbacks_;
    machine_->pMasterInfo = &master_;

    size_t gb = 0, tb = 0;
    for (int i = 0; i < info_->numGlobalParameters; ++i) gb += paramSize(*info_->Parameters[i]);
    for (int i = 0; i < info_->numTrackParameters; ++i) tb += paramSize(*info_->Parameters[info_->numGlobalParameters + i]);
    globals_.assign(gb, 0);
    trackVals_.assign(tb * std::max(1, info_->maxTracks), 0);
    attrs_.resize(std::max(0, info_->numAttributes));
    for (int i = 0; i < info_->numAttributes; ++i) attrs_[i] = info_->Attributes[i]->DefValue;

    if (machine_->GlobalVals) globals_.assign(static_cast<uint8_t *>(machine_->GlobalVals), static_cast<uint8_t *>(machine_->GlobalVals) + gb);
    if (machine_->TrackVals) trackVals_.assign(static_cast<uint8_t *>(machine_->TrackVals), static_cast<uint8_t *>(machine_->TrackVals) + trackVals_.size());
    if (machine_->AttrVals) for (int i = 0; i < info_->numAttributes; ++i) machine_->AttrVals[i] = attrs_[i];

    tracks_ = info_->minTracks;
    clearNoValues();
    std::fprintf(stderr, "[buzz] Init begin\n"); std::fflush(stderr);
    machineCall<void>(machine_, 1, static_cast<CMachineDataInput *>(nullptr));
    std::fprintf(stderr, "[buzz] Init end; AttributesChanged begin\n"); std::fflush(stderr);
    machineCall<void>(machine_, 7);
    std::fprintf(stderr, "[buzz] AttributesChanged end; SetNumTracks(%d) begin\n", tracks_); std::fflush(stderr);
    machineCall<void>(machine_, 9, tracks_);
    std::fprintf(stderr, "[buzz] SetNumTracks end; initial defaults begin\n"); std::fflush(stderr);

    // Buzz presents global parameter defaults on the first tick. Legacy
    // machines commonly initialize derived DSP state there and may enter Work
    // before any user parameter event arrives.
    if (machine_->GlobalVals) {
        auto *g = static_cast<uint8_t *>(machine_->GlobalVals);
        size_t off = 0;
        for (int i = 0; i < info_->numGlobalParameters; ++i) {
            const auto &p = *info_->Parameters[i];
            g[off] = static_cast<uint8_t>(p.DefValue & 0xff);
            if (paramSize(p) == 2) g[off + 1] = static_cast<uint8_t>((p.DefValue >> 8) & 0xff);
            off += paramSize(p);
        }
    }
    machineCall<void>(machine_, 2);
    clearNoValues();
    std::fprintf(stderr, "[buzz] initial defaults end\n"); std::fflush(stderr);
    return true;
}

void BuzzMachineInstance::destroy() {
    machineDestroy(machine_);
    machine_ = nullptr;
}

bool BuzzMachineInstance::load(const std::string &path, double r, int block, std::string &error) {
    close();
    std::fprintf(stderr, "[buzz] LoadLibraryEx %s begin\n", path.c_str()); std::fflush(stderr);
    module_ = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    std::fprintf(stderr, "[buzz] LoadLibraryEx end %p\n", static_cast<void *>(module_)); std::fflush(stderr);
    if (!module_) {
        error = classifyPluginLoadFailure(path, GetLastError());
        return false;
    }
    getInfo_ = reinterpret_cast<GetInfoFn>(GetProcAddress(module_, "GetInfo"));
    createMachine_ = reinterpret_cast<CreateMachineFn>(GetProcAddress(module_, "CreateMachine"));
    if (!getInfo_ || !createMachine_) {
        error = "Buzz machine lacks GetInfo/CreateMachine";
        close();
        return false;
    }
    std::fprintf(stderr, "[buzz] GetInfo begin\n"); std::fflush(stderr);
    info_ = getInfo_();
    std::fprintf(stderr, "[buzz] GetInfo end %p\n", static_cast<const void *>(info_)); std::fflush(stderr);
    if (!info_) {
        error = "Buzz GetInfo returned null";
        close();
        return false;
    }
    rate = r;
    block_ = std::min(block, 256);
    inPorts_ = info_->Type == MT_EFFECT ? 1 : 0;
    outPorts_ = 1;
    master_.SamplesPerSec = static_cast<int>(std::lround(rate));
    master_.BeatsPerMin = 126;
    master_.TicksPerBeat = 4;
    master_.SamplesPerTick = std::max(1, master_.SamplesPerSec * 60 / (master_.BeatsPerMin * master_.TicksPerBeat));
    master_.TicksPerSec = static_cast<float>(master_.SamplesPerSec) / master_.SamplesPerTick;
    mono_.assign(block_, 0.0f);
    return create(error);
}

void BuzzMachineInstance::close() {
    destroy();
    info_ = nullptr;
    getInfo_ = nullptr;
    createMachine_ = nullptr;
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
}

void BuzzMachineInstance::clearNoValues() {
    if (!machine_ || !info_) return;
    uint8_t *g = static_cast<uint8_t *>(machine_->GlobalVals);
    size_t off = 0;
    for (int i = 0; i < info_->numGlobalParameters; ++i) {
        const auto &p = *info_->Parameters[i];
        if (g) {
            g[off] = static_cast<uint8_t>(p.NoValue & 0xff);
            if (paramSize(p) == 2) g[off + 1] = static_cast<uint8_t>((p.NoValue >> 8) & 0xff);
        }
        off += paramSize(p);
    }
    uint8_t *t = static_cast<uint8_t *>(machine_->TrackVals);
    size_t stride = 0;
    for (int i = 0; i < info_->numTrackParameters; ++i) stride += paramSize(*info_->Parameters[info_->numGlobalParameters + i]);
    for (int tr = 0; tr < info_->maxTracks && t; ++tr) {
        off = 0;
        for (int i = 0; i < info_->numTrackParameters; ++i) {
            const auto &p = *info_->Parameters[info_->numGlobalParameters + i];
            auto *dst = t + tr * stride + off;
            dst[0] = static_cast<uint8_t>(p.NoValue & 0xff);
            if (paramSize(p) == 2) dst[1] = static_cast<uint8_t>((p.NoValue >> 8) & 0xff);
            off += paramSize(p);
        }
    }
}

void BuzzMachineInstance::process(const vstb_request &request, const float *inputs, float *outputs) {
    if (!machine_) return;
    const int frames = std::min<int>(request.frames, block_);
    master_.BeatsPerMin = static_cast<int>(std::lround(request.tempo > 0.0 ? request.tempo : 126.0));

    if (inPorts_ && inputs) {
        for (int i = 0; i < frames; ++i)
            mono_[i] = 32768.0f * 0.5f * (inputs[i] + inputs[block_ + i]);
    } else {
        std::fill(mono_.begin(), mono_.begin() + frames, 0.0f);
    }

    auto setRaw = [&](int track, int index, int raw) {
        if (track < 0) {
            if (index < 0 || index >= info_->numGlobalParameters || !machine_->GlobalVals) return;
            size_t off = 0;
            for (int p = 0; p < index; ++p) off += paramSize(*info_->Parameters[p]);
            const auto &param = *info_->Parameters[index];
            auto *dst = static_cast<uint8_t *>(machine_->GlobalVals) + off;
            dst[0] = static_cast<uint8_t>(raw & 0xff);
            if (paramSize(param) == 2) dst[1] = static_cast<uint8_t>((raw >> 8) & 0xff);
            return;
        }
        if (track >= info_->maxTracks || index < 0 || index >= info_->numTrackParameters || !machine_->TrackVals) return;
        size_t stride = 0, off = 0;
        for (int p = 0; p < info_->numTrackParameters; ++p) {
            const auto &param = *info_->Parameters[info_->numGlobalParameters + p];
            if (p < index) off += paramSize(param);
            stride += paramSize(param);
        }
        const auto &param = *info_->Parameters[info_->numGlobalParameters + index];
        auto *dst = static_cast<uint8_t *>(machine_->TrackVals) + track * stride + off;
        dst[0] = static_cast<uint8_t>(raw & 0xff);
        if (paramSize(param) == 2) dst[1] = static_cast<uint8_t>((raw >> 8) & 0xff);
    };

    auto render = [&](int from, int count) {
        if (count <= 0) return;
        master_.PosInTick = std::max(0, master_.PosInTick);
        const int mode = inPorts_ ? WM_READWRITE : WM_WRITE;
        machineCall<bool>(machine_, 3, mono_.data() + from, count, mode);
        for (int i = from; i < from + count; ++i) {
            const float s = mono_[i] / 32768.0f;
            outputs[i] = s;
            outputs[block_ + i] = s;
        }
        master_.PosInTick += count;
        if (master_.SamplesPerTick > 0) master_.PosInTick %= master_.SamplesPerTick;
    };

    int cursor = 0;
    for (uint32_t i = 0; i < request.eventCount; ++i) {
        const auto &ev = request.events[i];
        const int at = std::min<int>(frames, ev.offset);
        render(cursor, at - cursor);
        cursor = at;
        switch (ev.type) {
        case VSTB_EV_BUZZ_VALUE: {
            const int encodedTrack = int(ev.midi[0]) | (int(ev.midi[1]) << 8);
            setRaw(encodedTrack - 1, ev.index, static_cast<int>(std::lround(ev.value)));
            break;
        }
        case VSTB_EV_BUZZ_TICK:
            machineCall<void>(machine_, 2);
            clearNoValues();
            master_.PosInTick = 0;
            break;
        case VSTB_EV_BUZZ_TRACKS:
            tracks_ = std::clamp(static_cast<int>(std::lround(ev.value)), info_->minTracks, info_->maxTracks);
            machineCall<void>(machine_, 9, tracks_);
            break;
        case VSTB_EV_BUZZ_ATTR:
            if (ev.index < info_->numAttributes && machine_->AttrVals) {
                const auto &a = *info_->Attributes[ev.index];
                machine_->AttrVals[ev.index] = std::clamp(static_cast<int>(std::lround(ev.value)), a.MinValue, a.MaxValue);
                machineCall<void>(machine_, 7);
            }
            break;
        case VSTB_EV_BUZZ_MASTER:
            master_.TicksPerBeat = std::max(1, static_cast<int>(ev.index));
            master_.SamplesPerTick = std::max(1, static_cast<int>(std::lround(ev.value)));
            master_.TicksPerSec = static_cast<float>(master_.SamplesPerSec) / master_.SamplesPerTick;
            break;
        case VSTB_EV_BUZZ_STOP:
            machineCall<void>(machine_, 5);
            break;
        case VSTB_EV_MIDI:
            if ((ev.midi[0] & 0xf0) == 0x90)
                machineCall<void>(machine_, 12, int(ev.midi[0] & 0x0f), int(ev.midi[1]), int(ev.midi[2]));
            else if ((ev.midi[0] & 0xf0) == 0x80)
                machineCall<void>(machine_, 12, int(ev.midi[0] & 0x0f), int(ev.midi[1]), 0);
            break;
        case VSTB_EV_PARAM:
            if (ev.index < info_->numGlobalParameters) {
                const auto &p = *info_->Parameters[ev.index];
                setRaw(-1, ev.index, p.MinValue + static_cast<int>(std::lround(ev.value * (p.MaxValue - p.MinValue))));
            }
            break;
        default:
            break;
        }
    }
    render(cursor, frames - cursor);
}

std::string BuzzMachineInstance::describeJson() {
    if (!info_) return "{}";
    std::ostringstream s;
    s << "{\"format\":\"buzz\",\"name\":" << jsonString(info_->Name ? info_->Name : "Buzz machine")
      << ",\"product\":" << jsonString(info_->ShortName ? info_->ShortName : "")
      << ",\"vendor\":" << jsonString(info_->Author ? info_->Author : "")
      << ",\"vendorVersion\":" << info_->Version
      << ",\"synth\":" << (info_->Type == MT_GENERATOR ? "true" : "false")
      << ",\"buzzMachine\":true,\"minTracks\":" << info_->minTracks
      << ",\"maxTracks\":" << info_->maxTracks
      << ",\"inPorts\":[";
    if (inPorts_) s << "{\"name\":\"Main\"}";
    s << "],\"outPorts\":[{\"name\":\"Main\"}],\"latency\":0,\"programs\":[],\"params\":[";
    for (int i = 0; i < info_->numGlobalParameters; ++i) {
        if (i) s << ",";
        const auto &p = *info_->Parameters[i];
        s << "{\"name\":" << jsonString(p.Name ? p.Name : "")
          << ",\"label\":\"\",\"display\":" << jsonString(p.Description ? p.Description : "")
          << ",\"value\":" << (p.MaxValue == p.MinValue ? 0.0 : double(p.DefValue - p.MinValue) / double(p.MaxValue - p.MinValue))
          << ",\"buzzType\":" << static_cast<int>(p.Type)
          << ",\"minValue\":" << p.MinValue << ",\"maxValue\":" << p.MaxValue
          << ",\"noValue\":" << p.NoValue << ",\"defValue\":" << p.DefValue
          << ",\"flags\":" << p.Flags << ",\"description\":" << jsonString(p.Description ? p.Description : "") << "}";
    }
    s << "],\"trackParams\":[";
    for (int i = 0; i < info_->numTrackParameters; ++i) {
        if (i) s << ",";
        const auto &p = *info_->Parameters[info_->numGlobalParameters + i];
        s << "{\"name\":" << jsonString(p.Name ? p.Name : "")
          << ",\"buzzType\":" << static_cast<int>(p.Type)
          << ",\"minValue\":" << p.MinValue << ",\"maxValue\":" << p.MaxValue
          << ",\"noValue\":" << p.NoValue << ",\"defValue\":" << p.DefValue
          << ",\"flags\":" << p.Flags << ",\"description\":" << jsonString(p.Description ? p.Description : "") << "}";
    }
    s << "],\"attributes\":[";
    for (int i = 0; i < info_->numAttributes; ++i) {
        if (i) s << ",";
        const auto &a = *info_->Attributes[i];
        s << "{\"name\":" << jsonString(a.Name ? a.Name : "")
          << ",\"minValue\":" << a.MinValue << ",\"maxValue\":" << a.MaxValue
          << ",\"defValue\":" << a.DefValue << "}";
    }
    s << "]}";
    return s.str();
}

std::vector<uint8_t> BuzzMachineInstance::getState() {
    std::vector<uint8_t> out;
    if (!machine_) return out;
    MemoryOutput writer(out);
    machineCall<void>(machine_, 6, static_cast<CMachineDataOutput *>(&writer));
    return out;
}

void BuzzMachineInstance::initialise(const uint8_t *data, size_t size) {
    if (!machine_) return;
    if (data && size) {
        MemoryInput reader(data, size);
        machineCall<void>(machine_, 1, static_cast<CMachineDataInput *>(&reader));
    } else {
        machineCall<void>(machine_, 1, static_cast<CMachineDataInput *>(nullptr));
    }
    machineCall<void>(machine_, 7);
    machineCall<void>(machine_, 9, tracks_);
    clearNoValues();
}

bool BuzzMachineInstance::setState(const uint8_t *data, size_t size, std::string &error) {
    if (size > 1024 * 1024) {
        error = "Buzz native state exceeds 1 MiB";
        return false;
    }
    destroy();
    if (!create(error)) return false;
    initialise(data, size);
    return true;
}

void BuzzMachineInstance::reset() {
    std::string error;
    destroy();
    create(error);
}
