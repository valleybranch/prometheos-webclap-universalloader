#include "vst2_instance.h"

#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace {

// AEffect -> instance, for the host callback. Callbacks made inside
// VSTPluginMain arrive before the AEffect is known; they use tLoading.
std::mutex gInstancesMutex;
std::vector<std::pair<AEffect *, Vst2Instance *>> gInstances;
thread_local Vst2Instance *tLoading = nullptr;

Vst2Instance *instanceFor(AEffect *effect) {
    std::lock_guard<std::mutex> lock(gInstancesMutex);
    for (auto &[e, instance] : gInstances)
        if (e == effect) return instance;
    return tLoading;
}

intptr_t VST2_CALL hostCallback(AEffect *effect, int32_t opcode, int32_t, intptr_t, void *ptr, float) {
    Vst2Instance *self = instanceFor(effect);
    switch (opcode) {
    case audioMasterVersion: return 2400;
    case audioMasterCurrentId: return 0;
    case audioMasterGetTime: return self ? reinterpret_cast<intptr_t>(&self->time) : 0;
    case audioMasterGetSampleRate: return self ? static_cast<intptr_t>(self->rate) : 44100;
    case audioMasterGetBlockSize: return self ? self->block() : 256;
    case audioMasterGetCurrentProcessLevel: return 2; // realtime thread
    case audioMasterIOChanged:
        if (self) self->ioChanged = true;
        return 1;
    case audioMasterGetVendorString: std::strcpy(static_cast<char *>(ptr), "prometheos"); return 1;
    case audioMasterGetProductString: std::strcpy(static_cast<char *>(ptr), "vsthost (Boxedwine bridge)"); return 1;
    case audioMasterGetVendorVersion: return 1;
    case audioMasterCanDo: {
        const char *what = static_cast<const char *>(ptr);
        return what && (!std::strcmp(what, "sendVstEvents") || !std::strcmp(what, "sendVstMidiEvent") ||
                        !std::strcmp(what, "sendVstTimeInfo") || !std::strcmp(what, "receiveVstTimeInfo"))
                   ? 1
                   : 0;
    }
    default: return 0;
    }
}

struct StateHeader {
    char magic[4]; // "VSTS"
    uint32_t version;
    uint32_t kind; // 1: chunk (effGetChunk, bank), 2: parameter values
    int32_t program;
    uint32_t length;
};
enum { kStateChunk = 1, kStateParams = 2 };

} // namespace

Vst2Instance::~Vst2Instance() { close(); }

intptr_t Vst2Instance::dispatch(int32_t op, int32_t index, intptr_t value, void *ptr, float opt) {
    return effect_->dispatcher(effect_, op, index, value, ptr, opt);
}

bool Vst2Instance::load(const std::string &path, double sampleRate, int blockFrames, std::string &error) {
    path_ = path;
    rate = sampleRate;
    block_ = blockFrames;
    dll_ = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll_) {
        error = "LoadLibrary failed, error " + std::to_string(GetLastError());
        return false;
    }
    auto entry = reinterpret_cast<Vst2PluginMain>(GetProcAddress(dll_, "VSTPluginMain"));
    if (!entry) entry = reinterpret_cast<Vst2PluginMain>(GetProcAddress(dll_, "main"));
    if (!entry) {
        error = "no VSTPluginMain/main export (not a VST2 plugin?)";
        return false;
    }
    tLoading = this;
    effect_ = entry(hostCallback);
    tLoading = nullptr;
    if (!effect_ || effect_->magic != VST2_MAGIC) {
        effect_ = nullptr;
        error = "VSTPluginMain returned no AEffect";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(gInstancesMutex);
        gInstances.push_back({effect_, this});
    }
    dispatch(effOpen);
    dispatch(effSetSampleRate, 0, 0, nullptr, static_cast<float>(rate));
    dispatch(effSetBlockSize, 0, block_);

    inPorts_ = std::min<int>((std::max(0, effect_->numInputs) + 1) / 2, VSTB_MAX_IN_PORTS);
    outPorts_ = std::max(1, std::min<int>((std::max(0, effect_->numOutputs) + 1) / 2, VSTB_MAX_OUT_PORTS));
    const int ins = std::max(0, effect_->numInputs), outs = std::max(2, effect_->numOutputs);
    inBuf_.assign(ins, std::vector<float>(block_));
    outBuf_.assign(outs, std::vector<float>(block_));
    inPtr_.resize(ins);
    outPtr_.resize(outs);
    for (int i = 0; i < ins; ++i) inPtr_[i] = inBuf_[i].data();
    for (int i = 0; i < outs; ++i) outPtr_[i] = outBuf_[i].data();
    midi_.reserve(VSTB_MAX_EVENTS);
    eventStorage_.assign(sizeof(VstEvents) + VSTB_MAX_EVENTS * sizeof(VstEvent *), 0);

    time.sampleRate = rate;
    time.tempo = 120.0;
    time.timeSigNumerator = time.timeSigDenominator = 4;
    dispatch(effMainsChanged, 0, 1);
    dispatch(effStartProcess);
    return true;
}

void Vst2Instance::close() {
    if (effect_) {
        dispatch(effStopProcess);
        dispatch(effMainsChanged, 0, 0);
        dispatch(effClose);
        std::lock_guard<std::mutex> lock(gInstancesMutex);
        gInstances.erase(std::remove_if(gInstances.begin(), gInstances.end(),
                                        [&](auto &entry) { return entry.first == effect_; }),
                         gInstances.end());
        effect_ = nullptr;
    }
    // The module stays loaded for the life of the process, as when instances
    // of one plugin share it: unmapping a plugin's code made Boxedwine's
    // multithreaded JIT panic (KMemory::commitPreparedCodeInvalidation) on the
    // third unload of Dexed, and a later instance reuses the translated code.
    dll_ = nullptr;
}

void Vst2Instance::process(const vstb_request &request, const float *inputs, float *outputs) {
    const int frames = block_;
    time.samplePos = request.samplePos;
    time.ppqPos = request.ppqPos;
    time.tempo = request.tempo > 0 ? request.tempo : 120.0;
    time.flags = kVstTempoValid | kVstPpqPosValid | ((request.flags & VSTB_REQ_PLAYING) ? kVstTransportPlaying : 0);

    // Parameters and programs apply at the start of the block (VST2 has no
    // sample-accurate parameter changes); MIDI keeps its frame offsets.
    midi_.clear();
    const uint32_t count = std::min<uint32_t>(request.eventCount, VSTB_MAX_EVENTS);
    for (uint32_t i = 0; i < count; ++i) {
        const vstb_event &ev = request.events[i];
        if (ev.type == VSTB_EV_PARAM) {
            if (ev.index < effect_->numParams) effect_->setParameter(effect_, ev.index, ev.value);
        } else if (ev.type == VSTB_EV_PROGRAM) {
            dispatch(effSetProgram, 0, ev.index);
        } else if (ev.type == VSTB_EV_MIDI) {
            VstMidiEvent m{};
            m.type = kVstMidiType;
            m.byteSize = sizeof(VstMidiEvent);
            m.deltaFrames = static_cast<int32_t>(std::min<uint32_t>(ev.offset, frames - 1));
            m.flags = 1; // kVstMidiEventIsRealtime
            std::memcpy(m.midiData, ev.midi, 3);
            midi_.push_back(m);
        }
    }
    if (!midi_.empty()) {
        auto *events = reinterpret_cast<VstEvents *>(eventStorage_.data());
        events->numEvents = static_cast<int32_t>(midi_.size());
        events->reserved = 0;
        for (size_t i = 0; i < midi_.size(); ++i) events->events[i] = reinterpret_cast<VstEvent *>(&midi_[i]);
        dispatch(effProcessEvents, 0, 0, events);
    }

    // Stereo ports to plugin channels: port p carries channels 2p and 2p+1;
    // a mono input gets the L/R average, a mono output feeds both sides.
    const int ins = static_cast<int>(inBuf_.size());
    for (int i = 0; i < ins; ++i) {
        float *dst = inBuf_[i].data();
        const int port = i / 2;
        if (port >= inPorts_ || !(request.connectedMask & (1u << port))) {
            std::fill(dst, dst + frames, 0.0f);
        } else if (ins == 1) {
            const float *l = inputs, *r = inputs + frames;
            for (int f = 0; f < frames; ++f) dst[f] = 0.5f * (l[f] + r[f]);
        } else {
            std::memcpy(dst, inputs + (2 * port + i % 2) * frames, frames * sizeof(float));
        }
    }
    for (auto &b : outBuf_) std::fill(b.begin(), b.end(), 0.0f);
    if (effect_->flags & effFlagsCanReplacing) effect_->processReplacing(effect_, inPtr_.data(), outPtr_.data(), frames);
    else effect_->process(effect_, inPtr_.data(), outPtr_.data(), frames);

    const int outs = std::max(0, effect_->numOutputs);
    for (int p = 0; p < outPorts_; ++p) {
        for (int c = 0; c < 2; ++c) {
            const int channel = outs == 1 ? 0 : 2 * p + c;
            float *dst = outputs + (2 * p + c) * frames;
            if (channel < outs) std::memcpy(dst, outBuf_[channel].data(), frames * sizeof(float));
            else std::fill(dst, dst + frames, 0.0f);
        }
    }
}

void Vst2Instance::reset() {
    dispatch(effStopProcess);
    dispatch(effMainsChanged, 0, 0);
    dispatch(effMainsChanged, 0, 1);
    dispatch(effStartProcess);
}

std::string Vst2Instance::describeJson() {
    char text[256] = {};
    dispatch(effGetEffectName, 0, 0, text);
    std::string name = text;
    text[0] = 0;
    dispatch(effGetProductString, 0, 0, text);
    const std::string product = text;
    if (name.empty()) name = product;
    text[0] = 0;
    dispatch(effGetVendorString, 0, 0, text);
    const std::string vendor = text;

    std::string json = "{\"format\":\"vst2\",\"name\":\"" + jsonEscape(name) + "\",\"product\":\"" +
                       jsonEscape(product) + "\",\"vendor\":\"" + jsonEscape(vendor) + "\"";
    json += ",\"vendorVersion\":" + std::to_string(dispatch(effGetVendorVersion));
    json += ",\"uniqueId\":" + std::to_string(effect_->uniqueID);
    json += ",\"version\":" + std::to_string(effect_->version);
    json += std::string(",\"synth\":") + ((effect_->flags & effFlagsIsSynth) ? "true" : "false");
    json += std::string(",\"hasEditor\":") + ((effect_->flags & effFlagsHasEditor) ? "true" : "false");
    json += std::string(",\"programChunks\":") + ((effect_->flags & effFlagsProgramChunks) ? "true" : "false");
    json += ",\"numInputs\":" + std::to_string(effect_->numInputs);
    json += ",\"numOutputs\":" + std::to_string(effect_->numOutputs);
    json += ",\"latency\":" + std::to_string(effect_->initialDelay);
    json += ",\"blockFrames\":" + std::to_string(block_);
    json += ",\"sampleRate\":" + std::to_string(static_cast<int>(rate));
    auto ports = [&](int count, int32_t op, const char *firstKind, const char *otherKind) {
        std::string list = "[";
        for (int p = 0; p < count; ++p) {
            VstPinProperties pin{};
            std::string label;
            if (dispatch(op, 2 * p, 0, &pin) && pin.label[0]) label.assign(pin.label, strnlen(pin.label, sizeof pin.label));
            if (label.empty()) label = p == 0 ? "Main" : "Bus " + std::to_string(p + 1);
            list += std::string(p ? "," : "") + "{\"name\":\"" + jsonEscape(label) + "\",\"kind\":\"" +
                    (p == 0 ? firstKind : otherKind) + "\"}";
        }
        return list + "]";
    };
    json += ",\"inPorts\":" + ports(inPorts_, effGetInputProperties, "main", "sidechain");
    json += ",\"outPorts\":" + ports(outPorts_, effGetOutputProperties, "main", "aux");
    json += ",\"program\":" + std::to_string(dispatch(effGetProgram));
    json += ",\"programs\":[";
    for (int i = 0; i < effect_->numPrograms; ++i) {
        text[0] = 0;
        dispatch(effGetProgramNameIndexed, i, 0, text);
        json += std::string(i ? "," : "") + "\"" + jsonEscape(text) + "\"";
    }
    json += "],\"params\":[";
    for (int i = 0; i < effect_->numParams; ++i) {
        char pname[256] = {}, display[256] = {}, label[256] = {};
        dispatch(effGetParamName, i, 0, pname);
        dispatch(effGetParamDisplay, i, 0, display);
        dispatch(effGetParamLabel, i, 0, label);
        char value[32];
        std::snprintf(value, sizeof value, "%.6g", effect_->getParameter(effect_, i));
        json += std::string(i ? "," : "") + "{\"name\":\"" + jsonEscape(pname) + "\",\"label\":\"" + jsonEscape(label) +
                "\",\"display\":\"" + jsonEscape(display) + "\",\"value\":" + value + "}";
    }
    return json + "]}";
}

std::vector<uint8_t> Vst2Instance::getState() {
    StateHeader h{{'V', 'S', 'T', 'S'}, 1, kStateParams, static_cast<int32_t>(dispatch(effGetProgram)), 0};
    std::vector<uint8_t> data;
    if (effect_->flags & effFlagsProgramChunks) {
        void *chunk = nullptr;
        const intptr_t size = dispatch(effGetChunk, 0, 0, &chunk); // index 0: the whole bank
        if (size > 0 && chunk) {
            h.kind = kStateChunk;
            data.assign(static_cast<uint8_t *>(chunk), static_cast<uint8_t *>(chunk) + size);
        }
    }
    if (h.kind == kStateParams) {
        data.resize(effect_->numParams * sizeof(float));
        for (int i = 0; i < effect_->numParams; ++i) {
            const float v = effect_->getParameter(effect_, i);
            std::memcpy(data.data() + i * sizeof(float), &v, sizeof v);
        }
    }
    h.length = static_cast<uint32_t>(data.size());
    std::vector<uint8_t> blob(sizeof h + data.size());
    std::memcpy(blob.data(), &h, sizeof h);
    if (!data.empty()) std::memcpy(blob.data() + sizeof h, data.data(), data.size());
    return blob;
}

bool Vst2Instance::setState(const uint8_t *data, size_t size, std::string &error) {
    StateHeader h{};
    if (size < sizeof h || (std::memcpy(&h, data, sizeof h), std::memcmp(h.magic, "VSTS", 4)) ||
        sizeof h + h.length > size) {
        error = "not a vsthost state blob";
        return false;
    }
    const uint8_t *payload = data + sizeof h;
    if (h.kind == kStateChunk) {
        dispatch(effSetChunk, 0, h.length, const_cast<uint8_t *>(payload));
    } else if (h.kind == kStateParams) {
        if (h.program >= 0 && h.program < effect_->numPrograms) dispatch(effSetProgram, 0, h.program);
        const int n = std::min<int>(effect_->numParams, h.length / sizeof(float));
        for (int i = 0; i < n; ++i) {
            float v;
            std::memcpy(&v, payload + i * sizeof(float), sizeof v);
            effect_->setParameter(effect_, i, v);
        }
    } else {
        error = "unknown state kind " + std::to_string(h.kind);
        return false;
    }
    return true;
}
