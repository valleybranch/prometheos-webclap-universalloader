#include "buzz_instance.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace {
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
    machine_ = createMachine_ ? createMachine_() : nullptr;
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
    machine_->Init(nullptr);
    machine_->AttributesChanged();
    machine_->SetNumTracks(tracks_);
    return true;
}

void BuzzMachineInstance::destroy() {
    delete machine_;
    machine_ = nullptr;
}

bool BuzzMachineInstance::load(const std::string &path, double r, int block, std::string &error) {
    close();
    module_ = LoadLibraryA(path.c_str());
    if (!module_) {
        error = "LoadLibrary failed for Buzz machine";
        return false;
    }
    getInfo_ = reinterpret_cast<GetInfoFn>(GetProcAddress(module_, "GetInfo"));
    createMachine_ = reinterpret_cast<CreateMachineFn>(GetProcAddress(module_, "CreateMachine"));
    if (!getInfo_ || !createMachine_) {
        error = "Buzz machine lacks GetInfo/CreateMachine";
        close();
        return false;
    }
    info_ = getInfo_();
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
    master_.SamplesPerTick = std::max(1, master_.SamplesPerSec * 60 / std::max(1, master_.BeatsPerMin * master_.TicksPerBeat));
    master_.TicksPerSec = static_cast<float>(master_.SamplesPerSec) / master_.SamplesPerTick;
    master_.PosInTick = static_cast<int>(std::fmod(std::max(0.0, request.samplePos), static_cast<double>(master_.SamplesPerTick)));

    if (inPorts_ && inputs) {
        for (int i = 0; i < frames; ++i)
            mono_[i] = 32768.0f * 0.5f * (inputs[i] + inputs[block_ + i]);
    } else {
        std::fill(mono_.begin(), mono_.begin() + frames, 0.0f);
    }

    bool doTick = false;
    for (uint32_t i = 0; i < request.eventCount; ++i) {
        const auto &ev = request.events[i];
        if (ev.offset != 0) continue;
        if (ev.type == VSTB_EV_MIDI && (ev.midi[0] & 0xf0) == 0x90 && ev.midi[2]) {
            machine_->MidiNote(ev.midi[0] & 0x0f, ev.midi[1], ev.midi[2]);
        } else if (ev.type == VSTB_EV_MIDI && (ev.midi[0] & 0xf0) == 0x80) {
            machine_->MidiNote(ev.midi[0] & 0x0f, ev.midi[1], 0);
        } else if (ev.type == VSTB_EV_PARAM && ev.index < static_cast<uint16_t>(info_->numGlobalParameters)) {
            size_t off = 0;
            for (int p = 0; p < ev.index; ++p) off += paramSize(*info_->Parameters[p]);
            const auto &param = *info_->Parameters[ev.index];
            const int raw = param.MinValue + static_cast<int>(std::lround(ev.value * (param.MaxValue - param.MinValue)));
            auto *g = static_cast<uint8_t *>(machine_->GlobalVals);
            if (g) {
                g[off] = static_cast<uint8_t>(raw & 0xff);
                if (paramSize(param) == 2) g[off + 1] = static_cast<uint8_t>((raw >> 8) & 0xff);
                doTick = true;
            }
        }
    }
    if (doTick) {
        machine_->Tick();
        clearNoValues();
    }

    const int mode = inPorts_ ? WM_READWRITE : WM_WRITE;
    machine_->Work(mono_.data(), frames, mode);
    for (int i = 0; i < frames; ++i) {
        const float s = mono_[i] / 32768.0f;
        outputs[i] = s;
        outputs[block_ + i] = s;
    }
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
          << ",\"noValue\":" << p.NoValue << ",\"defValue\":" << p.DefValue << "}";
    }
    s << "],\"trackParams\":[";
    for (int i = 0; i < info_->numTrackParameters; ++i) {
        if (i) s << ",";
        const auto &p = *info_->Parameters[info_->numGlobalParameters + i];
        s << "{\"name\":" << jsonString(p.Name ? p.Name : "")
          << ",\"buzzType\":" << static_cast<int>(p.Type)
          << ",\"minValue\":" << p.MinValue << ",\"maxValue\":" << p.MaxValue
          << ",\"noValue\":" << p.NoValue << ",\"defValue\":" << p.DefValue << "}";
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
    machine_->Save(&writer);
    return out;
}

void BuzzMachineInstance::initialise(const uint8_t *data, size_t size) {
    if (!machine_) return;
    if (data && size) {
        MemoryInput reader(data, size);
        machine_->Init(&reader);
    } else {
        machine_->Init(nullptr);
    }
    machine_->AttributesChanged();
    machine_->SetNumTracks(tracks_);
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
