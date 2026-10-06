#include "vst3_instance.h"

#include "json.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

bool sameIid(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

std::string narrow(const TChar *text) {
    std::string out;
    for (; text && *text; ++text) {
        const unsigned c = static_cast<unsigned>(*text);
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

template <typename T> T *query(FUnknown *unknown, const TUID iid) {
    void *obj = nullptr;
    return unknown && unknown->queryInterface(iid, &obj) == kResultOk ? static_cast<T *>(obj) : nullptr;
}

// ---- Host objects the plugin may ask for ------------------------------------------

class HostAttributeList final : public IAttributeList {
  public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IAttributeList_iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --refs_;
        if (!left) delete this;
        return left;
    }
    tresult PLUGIN_API setInt(AttrID id, int64 value) override { return set(id, &value, sizeof value); }
    tresult PLUGIN_API getInt(AttrID id, int64 &value) override { return get(id, &value, sizeof value); }
    tresult PLUGIN_API setFloat(AttrID id, double value) override { return set(id, &value, sizeof value); }
    tresult PLUGIN_API getFloat(AttrID id, double &value) override { return get(id, &value, sizeof value); }
    tresult PLUGIN_API setString(AttrID id, const TChar *string) override {
        size_t n = 0;
        while (string && string[n]) ++n;
        return set(id, string, (n + 1) * sizeof(TChar));
    }
    tresult PLUGIN_API getString(AttrID id, TChar *string, uint32 sizeInBytes) override {
        auto it = values_.find(id ? id : "");
        if (it == values_.end() || !string || sizeInBytes < sizeof(TChar)) return kResultFalse;
        const size_t n = std::min<size_t>(it->second.size(), sizeInBytes - sizeof(TChar));
        std::memcpy(string, it->second.data(), n);
        string[n / sizeof(TChar)] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void *data, uint32 sizeInBytes) override {
        return set(id, data, sizeInBytes);
    }
    tresult PLUGIN_API getBinary(AttrID id, const void *&data, uint32 &sizeInBytes) override {
        auto it = values_.find(id ? id : "");
        if (it == values_.end()) return kResultFalse;
        data = it->second.data();
        sizeInBytes = static_cast<uint32>(it->second.size());
        return kResultOk;
    }

  private:
    tresult set(AttrID id, const void *data, size_t size) {
        auto &v = values_[id ? id : ""];
        v.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        return kResultOk;
    }
    tresult get(AttrID id, void *out, size_t size) {
        auto it = values_.find(id ? id : "");
        if (it == values_.end() || it->second.size() != size) return kResultFalse;
        std::memcpy(out, it->second.data(), size);
        return kResultOk;
    }
    std::atomic<uint32> refs_{1};
    std::map<std::string, std::vector<uint8_t>> values_;
};

class HostMessage final : public IMessage {
  public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IMessage_iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --refs_;
        if (!left) delete this;
        return left;
    }
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList *PLUGIN_API getAttributes() override { return attributes_; }
    ~HostMessage() { attributes_->release(); }

  private:
    std::atomic<uint32> refs_{1};
    std::string id_;
    HostAttributeList *attributes_ = new HostAttributeList;
};

class HostApplication final : public IHostApplication {
  public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IHostApplication_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getName(String128 name) override {
        const char *n = "vsthost (Boxedwine bridge)";
        int i = 0;
        for (; n[i]; ++i) name[i] = static_cast<char16>(n[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **obj) override {
        if (sameIid(cid, IMessage_iid) && sameIid(iid, IMessage_iid)) {
            *obj = static_cast<IMessage *>(new HostMessage);
            return kResultOk;
        }
        if (sameIid(cid, IAttributeList_iid) && sameIid(iid, IAttributeList_iid)) {
            *obj = static_cast<IAttributeList *>(new HostAttributeList);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

HostApplication gHostApp;

class MemoryStream final : public IBStream {
  public:
    std::vector<uint8_t> data;
    int64 pos = 0;
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IBStream_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API read(void *buffer, int32 numBytes, int32 *numBytesRead) override {
        const int64 n = std::max<int64>(0, std::min<int64>(numBytes, static_cast<int64>(data.size()) - pos));
        if (n) std::memcpy(buffer, data.data() + pos, static_cast<size_t>(n));
        pos += n;
        if (numBytesRead) *numBytesRead = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buffer, int32 numBytes, int32 *numBytesWritten) override {
        if (numBytes < 0) return kInvalidArgument;
        if (pos + numBytes > static_cast<int64>(data.size())) data.resize(static_cast<size_t>(pos + numBytes));
        std::memcpy(data.data() + pos, buffer, static_cast<size_t>(numBytes));
        pos += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset, int32 mode, int64 *result) override {
        int64 base = mode == kIBSeekCur ? pos : mode == kIBSeekEnd ? static_cast<int64>(data.size()) : 0;
        if (base + offset < 0) return kInvalidArgument;
        pos = base + offset;
        if (result) *result = pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *result) override {
        if (result) *result = pos;
        return kResultOk;
    }
};

// Preallocated, so process() does not allocate.
class EventList final : public IEventList {
  public:
    explicit EventList(size_t capacity) : events_(capacity) {}
    void clear() { count_ = 0; }
    bool add(const Event &e) {
        if (count_ >= events_.size()) return false;
        events_[count_++] = e;
        return true;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IEventList_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return static_cast<int32>(count_); }
    tresult PLUGIN_API getEvent(int32 index, Event &e) override {
        if (index < 0 || static_cast<size_t>(index) >= count_) return kInvalidArgument;
        e = events_[index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event &e) override { return add(e) ? kResultOk : kResultFalse; }

  private:
    std::vector<Event> events_;
    size_t count_ = 0;
};

class ParamQueue final : public IParamValueQueue {
  public:
    static constexpr int kPoints = 32;
    ParamID id = 0;
    int32 count = 0;
    int32 offsets[kPoints] = {};
    ParamValue values[kPoints] = {};
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IParamValueQueue_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32 &sampleOffset, ParamValue &value) override {
        if (index < 0 || index >= count) return kInvalidArgument;
        sampleOffset = offsets[index];
        value = values[index];
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32 &index) override {
        // Points stay in offset order; a second point at one offset replaces the first.
        if (count && offsets[count - 1] >= sampleOffset) {
            index = count - 1;
            values[index] = value;
            return kResultOk;
        }
        if (count >= kPoints) return kResultFalse;
        index = count++;
        offsets[index] = sampleOffset;
        values[index] = value;
        return kResultOk;
    }
};

class ParamChanges final : public IParameterChanges {
  public:
    static constexpr int kQueues = 128;
    void clear() { used_ = 0; }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IParameterChanges_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return used_; }
    IParamValueQueue *PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < used_ ? &queues_[index] : nullptr;
    }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &index) override {
        for (int32 i = 0; i < used_; ++i)
            if (queues_[i].id == id) {
                index = i;
                return &queues_[i];
            }
        if (used_ >= kQueues) return nullptr;
        index = used_++;
        queues_[index].id = id;
        queues_[index].count = 0;
        return &queues_[index];
    }
    void add(ParamID id, int32 offset, ParamValue value) {
        int32 index = 0, point = 0;
        if (IParamValueQueue *q = addParameterData(id, index)) q->addPoint(offset, value, point);
    }
    ParamQueue &queue(int32 index) { return queues_[index]; }

  private:
    ParamQueue queues_[kQueues];
    int32 used_ = 0;
};

class ComponentHandler final : public IComponentHandler {
  public:
    explicit ComponentHandler(Vst3Instance &owner) : owner_(owner) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        if (sameIid(iid, FUnknown_iid) || sameIid(iid, IComponentHandler_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    // Edits come from a plugin editor, which this host does not open.
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if (flags & (kLatencyChanged | kIoChanged)) owner_.ioChanged = true;
        return kResultOk;
    }

  private:
    Vst3Instance &owner_;
};

struct StateHeader {
    char magic[4]; // "VST3"
    uint32_t version;
    uint32_t componentBytes;
    uint32_t controllerBytes;
};

// InitDll once per module: modules stay loaded and instances share them.
std::mutex gModulesMutex;
std::set<HMODULE> gInitialized;

} // namespace

struct Vst3Instance::Impl {
    explicit Impl(Vst3Instance &owner) : handler(owner), events(2 * VSTB_MAX_EVENTS + 16 * 128) {}

    HMODULE dll = nullptr;
    IPluginFactory *factory = nullptr;
    std::string name, vendor, version, subCategories;
    IComponent *component = nullptr;
    IAudioProcessor *processor = nullptr;
    IEditController *controller = nullptr;
    bool separateController = false;
    IConnectionPoint *componentPoint = nullptr, *controllerPoint = nullptr;
    IMidiMapping *midiMapping = nullptr;
    ComponentHandler handler;
    bool active = false;

    int32 audioIns = 0, audioOuts = 0;
    bool eventIn = false;
    std::vector<int32> inChannels, outChannels;
    std::vector<std::vector<float>> inStore, outStore;
    std::vector<std::vector<float *>> inPtrs, outPtrs;
    std::vector<AudioBusBuffers> inBus, outBus;

    EventList events;
    ParamChanges inChanges, outChanges;
    ProcessContext context{};

    std::vector<ParamID> params;     // VSTB_EV_PARAM index -> id
    ParamID programParam = kNoParamId;
    int32 programSteps = 0;
    bool held[16][128] = {};

    void addNote(bool on, int channel, int pitch, float velocity, int offset) {
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = offset;
        if (on) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = static_cast<int16>(channel);
            e.noteOn.pitch = static_cast<int16>(pitch);
            e.noteOn.velocity = velocity;
            e.noteOn.noteId = -1;
        } else {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = static_cast<int16>(channel);
            e.noteOff.pitch = static_cast<int16>(pitch);
            e.noteOff.velocity = velocity;
            e.noteOff.noteId = -1;
        }
        held[channel][pitch] = on;
        events.add(e);
    }

    // A MIDI controller (CC 0..127, kAfterTouch, kPitchBend) through the
    // plugin's own MIDI mapping, as a parameter change.
    void addController(int channel, CtrlNumber number, double value, int offset) {
        ParamID id = kNoParamId;
        if (midiMapping && midiMapping->getMidiControllerAssignment(0, static_cast<int16>(channel), number, id) == kResultOk &&
            id != kNoParamId)
            inChanges.add(id, offset, value);
    }
};

Vst3Instance::Vst3Instance() : impl_(std::make_unique<Impl>(*this)) {}

Vst3Instance::~Vst3Instance() { close(); }

bool Vst3Instance::load(const std::string &path, double sampleRate, int blockFrames, std::string &error) {
    Impl &m = *impl_;
    rate = sampleRate;
    block_ = blockFrames;
    m.dll = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m.dll) {
        error = "LoadLibrary failed, error " + std::to_string(GetLastError());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(gModulesMutex);
        if (gInitialized.insert(m.dll).second) {
            using InitFn = bool (*)();
            if (auto init = reinterpret_cast<InitFn>(GetProcAddress(m.dll, "InitDll"))) init();
        }
    }
    using FactoryFn = IPluginFactory *(PLUGIN_API *)();
    auto getFactory = reinterpret_cast<FactoryFn>(GetProcAddress(m.dll, "GetPluginFactory"));
    m.factory = getFactory ? getFactory() : nullptr;
    if (!m.factory) {
        error = "GetPluginFactory returned no factory";
        return false;
    }
    PFactoryInfo factoryInfo{};
    m.factory->getFactoryInfo(&factoryInfo);
    m.vendor = factoryInfo.vendor;

    PClassInfo info{};
    int32 classIndex = -1;
    for (int32 i = 0; i < m.factory->countClasses(); ++i) {
        if (m.factory->getClassInfo(i, &info) == kResultOk && !std::strcmp(info.category, kVstAudioEffectClass)) {
            classIndex = i;
            break;
        }
    }
    if (classIndex < 0) {
        error = "no audio module class in the VST3 factory";
        return false;
    }
    m.name = info.name;
    if (auto *factory2 = query<IPluginFactory2>(m.factory, IPluginFactory2_iid)) {
        PClassInfo2 info2{};
        if (factory2->getClassInfo2(classIndex, &info2) == kResultOk) {
            if (info2.vendor[0]) m.vendor = info2.vendor;
            m.version = info2.version;
            m.subCategories = info2.subCategories;
        }
        factory2->release();
    }

    void *obj = nullptr;
    if (m.factory->createInstance(info.cid, IComponent_iid, &obj) != kResultOk || !obj) {
        error = "createInstance(IComponent) failed";
        return false;
    }
    m.component = static_cast<IComponent *>(obj);
    if (m.component->initialize(&gHostApp) != kResultOk) {
        error = "IComponent::initialize failed";
        return false;
    }
    m.processor = query<IAudioProcessor>(m.component, IAudioProcessor_iid);
    if (!m.processor) {
        error = "the component has no IAudioProcessor";
        return false;
    }

    // The controller: the component itself, or a class of its own connected
    // to it through IConnectionPoint and given the component's state.
    m.controller = query<IEditController>(m.component, IEditController_iid);
    TUID controllerCid;
    if (!m.controller && m.component->getControllerClassId(controllerCid) == kResultOk &&
        m.factory->createInstance(controllerCid, IEditController_iid, &obj) == kResultOk && obj) {
        m.controller = static_cast<IEditController *>(obj);
        m.separateController = true;
        if (m.controller->initialize(&gHostApp) != kResultOk) {
            m.controller->release();
            m.controller = nullptr;
            m.separateController = false;
        }
    }
    if (m.separateController) {
        m.componentPoint = query<IConnectionPoint>(m.component, IConnectionPoint_iid);
        m.controllerPoint = query<IConnectionPoint>(m.controller, IConnectionPoint_iid);
        if (m.componentPoint && m.controllerPoint) {
            m.componentPoint->connect(m.controllerPoint);
            m.controllerPoint->connect(m.componentPoint);
        }
        MemoryStream state;
        if (m.component->getState(&state) == kResultOk) {
            state.pos = 0;
            m.controller->setComponentState(&state);
        }
    }
    if (m.controller) {
        m.controller->setComponentHandler(&m.handler);
        m.midiMapping = query<IMidiMapping>(m.controller, IMidiMapping_iid);
        for (int32 i = 0; i < m.controller->getParameterCount(); ++i) {
            ParameterInfo pi{};
            if (m.controller->getParameterInfo(i, pi) != kResultOk) continue;
            if (pi.flags & ParameterInfo::kIsProgramChange) {
                if (m.programParam == kNoParamId) {
                    m.programParam = pi.id;
                    m.programSteps = pi.stepCount;
                }
                continue;
            }
            // Hidden parameters, and JUCE's stand-ins for MIDI CCs (it maps
            // them through IMidiMapping, which this host uses instead).
            if (pi.flags & ParameterInfo::kIsHidden) continue;
            if (narrow(pi.title).rfind("MIDI CC ", 0) == 0) continue;
            m.params.push_back(pi.id);
        }
    }

    // Busses: every audio bus stereo where the plugin agrees; port p is bus p.
    m.audioIns = m.component->getBusCount(kAudio, kInput);
    m.audioOuts = m.component->getBusCount(kAudio, kOutput);
    m.eventIn = m.component->getBusCount(kEvent, kInput) > 0;
    std::vector<SpeakerArrangement> inArr(m.audioIns, SpeakerArr::kStereo), outArr(m.audioOuts, SpeakerArr::kStereo);
    m.processor->setBusArrangements(inArr.data(), m.audioIns, outArr.data(), m.audioOuts);
    auto channels = [&](BusDirection dir, int32 index) {
        BusInfo bi{};
        return m.component->getBusInfo(kAudio, dir, index, bi) == kResultOk ? std::max<int32>(1, bi.channelCount) : 2;
    };
    for (int32 i = 0; i < m.audioIns; ++i) {
        m.inChannels.push_back(channels(kInput, i));
        m.component->activateBus(kAudio, kInput, i, true);
    }
    for (int32 i = 0; i < m.audioOuts; ++i) {
        m.outChannels.push_back(channels(kOutput, i));
        m.component->activateBus(kAudio, kOutput, i, true);
    }
    if (m.eventIn) m.component->activateBus(kEvent, kInput, 0, true);
    inPorts_ = std::min<int>(m.audioIns, VSTB_MAX_IN_PORTS);
    outPorts_ = std::max(1, std::min<int>(m.audioOuts, VSTB_MAX_OUT_PORTS));

    auto buffers = [&](const std::vector<int32> &counts, std::vector<std::vector<float>> &store,
                       std::vector<std::vector<float *>> &ptrs, std::vector<AudioBusBuffers> &bus) {
        size_t total = 0;
        for (int32 c : counts) total += c;
        store.assign(total, std::vector<float>(block_));
        ptrs.resize(counts.size());
        bus.resize(counts.size());
        size_t k = 0;
        for (size_t b = 0; b < counts.size(); ++b) {
            ptrs[b].clear();
            for (int32 c = 0; c < counts[b]; ++c) ptrs[b].push_back(store[k++].data());
            bus[b] = AudioBusBuffers{};
            bus[b].numChannels = counts[b];
            bus[b].channelBuffers32 = ptrs[b].data();
        }
    };
    buffers(m.inChannels, m.inStore, m.inPtrs, m.inBus);
    buffers(m.outChannels, m.outStore, m.outPtrs, m.outBus);

    ProcessSetup setup{kRealtime, kSample32, block_, rate};
    if (m.processor->canProcessSampleSize(kSample32) != kResultOk || m.processor->setupProcessing(setup) != kResultOk) {
        error = "the plugin cannot process 32-bit samples at this rate and block size";
        return false;
    }
    m.context.sampleRate = rate;
    m.context.tempo = 120.0;
    m.context.timeSigNumerator = m.context.timeSigDenominator = 4;
    m.component->setActive(true);
    m.processor->setProcessing(true);
    m.active = true;
    return true;
}

void Vst3Instance::close() {
    Impl &m = *impl_;
    if (m.active) {
        m.processor->setProcessing(false);
        m.component->setActive(false);
        m.active = false;
    }
    if (m.componentPoint && m.controllerPoint) {
        m.componentPoint->disconnect(m.controllerPoint);
        m.controllerPoint->disconnect(m.componentPoint);
    }
    if (m.componentPoint) m.componentPoint->release();
    if (m.controllerPoint) m.controllerPoint->release();
    m.componentPoint = m.controllerPoint = nullptr;
    if (m.midiMapping) m.midiMapping->release();
    m.midiMapping = nullptr;
    if (m.controller) {
        if (m.separateController) m.controller->terminate();
        m.controller->release();
        m.controller = nullptr;
    }
    if (m.processor) m.processor->release();
    m.processor = nullptr;
    if (m.component) {
        m.component->terminate();
        m.component->release();
        m.component = nullptr;
    }
    // The module stays loaded for the life of the process (see Vst2Instance::close).
    m.dll = nullptr;
}

void Vst3Instance::reset() {
    Impl &m = *impl_;
    m.processor->setProcessing(false);
    m.component->setActive(false);
    m.component->setActive(true);
    m.processor->setProcessing(true);
    std::memset(m.held, 0, sizeof m.held);
}

int Vst3Instance::latency() const { return impl_->processor ? static_cast<int>(impl_->processor->getLatencySamples()) : 0; }

void Vst3Instance::process(const vstb_request &request, const float *inputs, float *outputs) {
    Impl &m = *impl_;
    const int frames = block_;
    m.context.projectTimeSamples = static_cast<TSamples>(request.samplePos);
    m.context.projectTimeMusic = request.ppqPos;
    m.context.tempo = request.tempo > 0 ? request.tempo : 120.0;
    m.context.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid |
                      ProcessContext::kTimeSigValid | ((request.flags & VSTB_REQ_PLAYING) ? ProcessContext::kPlaying : 0);

    m.events.clear();
    m.inChanges.clear();
    m.outChanges.clear();
    const uint32_t count = std::min<uint32_t>(request.eventCount, VSTB_MAX_EVENTS);
    auto program = [&](int index, int offset) {
        if (m.programParam == kNoParamId) return;
        const double value = m.programSteps > 0 ? std::min(1.0, static_cast<double>(index) / m.programSteps) : 0.0;
        m.inChanges.add(m.programParam, offset, value);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const vstb_event &ev = request.events[i];
        const int offset = static_cast<int>(std::min<uint32_t>(ev.offset, frames - 1));
        if (ev.type == VSTB_EV_PARAM) {
            if (ev.index < m.params.size()) m.inChanges.add(m.params[ev.index], offset, ev.value);
        } else if (ev.type == VSTB_EV_PROGRAM) {
            program(static_cast<int>(ev.index), offset);
        } else if (ev.type == VSTB_EV_MIDI) {
            const int status = ev.midi[0] & 0xF0, channel = ev.midi[0] & 0x0F;
            const int d1 = ev.midi[1] & 0x7F, d2 = ev.midi[2] & 0x7F;
            switch (status) {
            case 0x90:
                if (d2) {
                    if (m.eventIn) m.addNote(true, channel, d1, d2 / 127.0f, offset);
                    break;
                }
                [[fallthrough]];
            case 0x80:
                if (m.eventIn) m.addNote(false, channel, d1, d2 / 127.0f, offset);
                break;
            case 0xA0: {
                Event e{};
                e.type = Event::kPolyPressureEvent;
                e.sampleOffset = offset;
                e.polyPressure.channel = static_cast<int16>(channel);
                e.polyPressure.pitch = static_cast<int16>(d1);
                e.polyPressure.pressure = d2 / 127.0f;
                e.polyPressure.noteId = -1;
                if (m.eventIn) m.events.add(e);
                break;
            }
            case 0xB0:
                if (d1 == 120 || d1 == 123) { // all sound / all notes off
                    for (int p = 0; p < 128; ++p)
                        if (m.held[channel][p]) m.addNote(false, channel, p, 0.0f, offset);
                } else {
                    m.addController(channel, static_cast<CtrlNumber>(d1), d2 / 127.0, offset);
                }
                break;
            case 0xC0: program(d1, offset); break;
            case 0xD0: m.addController(channel, kAfterTouch, d1 / 127.0, offset); break;
            case 0xE0: m.addController(channel, kPitchBend, ((d2 << 7) | d1) / 16383.0, offset); break;
            default: break;
            }
        }
    }

    // Port p is bus p: a mono bus gets the L/R average and feeds both sides.
    for (int32 b = 0; b < m.audioIns; ++b) {
        const bool connected = b < inPorts_ && (request.connectedMask & (1u << b));
        for (int32 c = 0; c < m.inChannels[b]; ++c) {
            float *dst = m.inPtrs[b][c];
            if (!connected || c >= 2) std::fill(dst, dst + frames, 0.0f);
            else if (m.inChannels[b] == 1) {
                const float *l = inputs + 2 * b * frames, *r = l + frames;
                for (int f = 0; f < frames; ++f) dst[f] = 0.5f * (l[f] + r[f]);
            } else std::memcpy(dst, inputs + (2 * b + c) * frames, frames * sizeof(float));
        }
        m.inBus[b].silenceFlags = 0;
    }
    for (int32 b = 0; b < m.audioOuts; ++b) {
        for (int32 c = 0; c < m.outChannels[b]; ++c) std::fill(m.outPtrs[b][c], m.outPtrs[b][c] + frames, 0.0f);
        m.outBus[b].silenceFlags = 0;
    }

    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = m.audioIns;
    data.numOutputs = m.audioOuts;
    data.inputs = m.inBus.empty() ? nullptr : m.inBus.data();
    data.outputs = m.outBus.empty() ? nullptr : m.outBus.data();
    data.inputParameterChanges = &m.inChanges;
    data.outputParameterChanges = &m.outChanges;
    data.inputEvents = m.eventIn ? &m.events : nullptr;
    data.processContext = &m.context;
    m.processor->process(data);

    for (int p = 0; p < outPorts_; ++p) {
        for (int c = 0; c < 2; ++c) {
            float *dst = outputs + (2 * p + c) * frames;
            if (p < m.audioOuts) {
                const int32 n = m.outChannels[p];
                std::memcpy(dst, m.outPtrs[p][n == 1 ? 0 : c], frames * sizeof(float));
            } else std::fill(dst, dst + frames, 0.0f);
        }
    }

    // The controller follows every change, so describe and state read it back.
    if (m.controller && m.separateController) {
        for (ParamChanges *changes : {&m.inChanges, &m.outChanges})
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                ParamQueue &q = changes->queue(i);
                if (q.count) m.controller->setParamNormalized(q.id, q.values[q.count - 1]);
            }
    }
}

std::string Vst3Instance::describeJson() {
    Impl &m = *impl_;
    const bool synth = m.eventIn && (m.audioIns == 0 || m.subCategories.find("Instrument") != std::string::npos);
    int numIns = 0, numOuts = 0;
    for (int32 c : m.inChannels) numIns += c;
    for (int32 c : m.outChannels) numOuts += c;
    std::string json = "{\"format\":\"vst3\",\"name\":\"" + jsonEscape(m.name) + "\",\"product\":\"" + jsonEscape(m.name) +
                       "\",\"vendor\":\"" + jsonEscape(m.vendor) + "\",\"versionString\":\"" + jsonEscape(m.version) +
                       "\",\"subCategories\":\"" + jsonEscape(m.subCategories) + "\"";
    json += ",\"vendorVersion\":0,\"uniqueId\":0,\"version\":0";
    json += std::string(",\"synth\":") + (synth ? "true" : "false");
    json += ",\"hasEditor\":false,\"programChunks\":true";
    json += ",\"numInputs\":" + std::to_string(numIns) + ",\"numOutputs\":" + std::to_string(numOuts);
    json += ",\"latency\":" + std::to_string(latency());
    json += ",\"blockFrames\":" + std::to_string(block_);
    json += ",\"sampleRate\":" + std::to_string(static_cast<int>(rate));
    auto ports = [&](BusDirection dir, int count, const char *firstKind, const char *otherKind) {
        std::string list = "[";
        for (int p = 0; p < count; ++p) {
            BusInfo bi{};
            std::string label;
            if (p < m.component->getBusCount(kAudio, dir) && m.component->getBusInfo(kAudio, dir, p, bi) == kResultOk)
                label = narrow(bi.name);
            if (label.empty()) label = p == 0 ? "Main" : "Bus " + std::to_string(p + 1);
            list += std::string(p ? "," : "") + "{\"name\":\"" + jsonEscape(label) + "\",\"kind\":\"" +
                    (p == 0 ? firstKind : otherKind) + "\"}";
        }
        return list + "]";
    };
    json += ",\"inPorts\":" + ports(kInput, inPorts_, "main", "sidechain");
    json += ",\"outPorts\":" + ports(kOutput, outPorts_, "main", "aux");
    json += ",\"program\":";
    if (m.controller && m.programParam != kNoParamId) {
        json += std::to_string(static_cast<int>(std::lround(m.controller->getParamNormalized(m.programParam) * m.programSteps)));
        json += ",\"programs\":[";
        for (int32 i = 0; i <= std::min<int32>(m.programSteps, 1023); ++i) {
            String128 text{};
            const double v = m.programSteps > 0 ? static_cast<double>(i) / m.programSteps : 0.0;
            std::string label = m.controller->getParamStringByValue(m.programParam, v, text) == kResultOk ? narrow(text) : "";
            json += std::string(i ? "," : "") + "\"" + jsonEscape(label) + "\"";
        }
        json += "]";
    } else {
        json += "0,\"programs\":[]";
    }
    json += ",\"params\":[";
    for (size_t i = 0; i < m.params.size(); ++i) {
        ParameterInfo pi{};
        for (int32 k = 0; m.controller && k < m.controller->getParameterCount(); ++k)
            if (m.controller->getParameterInfo(k, pi) == kResultOk && pi.id == m.params[i]) break;
        const double value = m.controller ? m.controller->getParamNormalized(m.params[i]) : 0.0;
        String128 text{};
        const std::string display =
            m.controller && m.controller->getParamStringByValue(m.params[i], value, text) == kResultOk ? narrow(text) : "";
        char number[32];
        std::snprintf(number, sizeof number, "%.6g", value);
        json += std::string(i ? "," : "") + "{\"name\":\"" + jsonEscape(narrow(pi.title)) + "\",\"label\":\"" +
                jsonEscape(narrow(pi.units)) + "\",\"display\":\"" + jsonEscape(display) + "\",\"value\":" + number +
                ",\"id\":" + std::to_string(pi.id) + ",\"steps\":" + std::to_string(pi.stepCount) + "}";
    }
    return json + "]}";
}

std::vector<uint8_t> Vst3Instance::getState() {
    Impl &m = *impl_;
    MemoryStream component, controller;
    m.component->getState(&component);
    if (m.controller && m.separateController) m.controller->getState(&controller);
    StateHeader h{{'V', 'S', 'T', '3'}, 1, static_cast<uint32_t>(component.data.size()),
                  static_cast<uint32_t>(controller.data.size())};
    std::vector<uint8_t> blob(sizeof h + component.data.size() + controller.data.size());
    std::memcpy(blob.data(), &h, sizeof h);
    if (!component.data.empty()) std::memcpy(blob.data() + sizeof h, component.data.data(), component.data.size());
    if (!controller.data.empty())
        std::memcpy(blob.data() + sizeof h + component.data.size(), controller.data.data(), controller.data.size());
    return blob;
}

bool Vst3Instance::setState(const uint8_t *data, size_t size, std::string &error) {
    Impl &m = *impl_;
    StateHeader h{};
    if (size < sizeof h || (std::memcpy(&h, data, sizeof h), std::memcmp(h.magic, "VST3", 4)) ||
        sizeof h + static_cast<size_t>(h.componentBytes) + h.controllerBytes > size) {
        error = "not a vsthost VST3 state blob";
        return false;
    }
    MemoryStream component;
    component.data.assign(data + sizeof h, data + sizeof h + h.componentBytes);
    if (m.component->setState(&component) != kResultOk) {
        error = "IComponent::setState failed";
        return false;
    }
    if (m.controller) {
        component.pos = 0;
        m.controller->setComponentState(&component);
        if (m.separateController && h.controllerBytes) {
            MemoryStream controller;
            const uint8_t *p = data + sizeof h + h.componentBytes;
            controller.data.assign(p, p + h.controllerBytes);
            m.controller->setState(&controller);
        }
    }
    return true;
}
