#include "../../host/buzz_abi.h"

#include <cstdint>

namespace {

CMachineParameter kLevel = {
    pt_byte,
    "Level",
    "Startup level",
    0,
    254,
    255,
    2,
    123,
};
CMachineParameter const *kParameters[] = { &kLevel };

CMachineInfo kInfo = {
    MT_GENERATOR,
    15,
    0,
    1,
    1,
    1,
    0,
    kParameters,
    0,
    nullptr,
    "Buzz Startup Fixture",
    "StartupFixture",
    "PrometheOS",
    "",
    nullptr,
};

struct FixtureMachine {
    CMachineInterface base{};
    std::uint8_t global = 0;
    bool ready = false;
};

FixtureMachine gMachine;
void *gVtable[19]{};

FixtureMachine *fixture(CMachineInterface *base) {
    return reinterpret_cast<FixtureMachine *>(base);
}

void __attribute__((thiscall)) destroyMachine(CMachineInterface *, unsigned) {}

void __attribute__((thiscall)) initMachine(CMachineInterface *base, CMachineDataInput *) {
    fixture(base)->ready = false;
}

void __attribute__((thiscall)) tickMachine(CMachineInterface *base) {
    auto *m = fixture(base);
    if (m->global != static_cast<std::uint8_t>(kLevel.NoValue)) {
        m->ready = true;
    }
}

bool __attribute__((thiscall)) workMachine(CMachineInterface *base, float *samples, int count, int) {
    auto *m = fixture(base);
    const float value = m->ready ? 1000.0f : 0.0f;
    for (int i = 0; i < count; ++i) samples[i] = value;
    return m->ready;
}

void __attribute__((thiscall)) saveMachine(CMachineInterface *, CMachineDataOutput *) {}
void __attribute__((thiscall)) attributesChanged(CMachineInterface *) {}
void __attribute__((thiscall)) setNumTracks(CMachineInterface *, int) {}
void __attribute__((thiscall)) midiNote(CMachineInterface *, int, int, int) {}
void __attribute__((thiscall)) noop(CMachineInterface *) {}

void initVtable() {
    for (void *&entry : gVtable) entry = reinterpret_cast<void *>(&noop);
    gVtable[0] = reinterpret_cast<void *>(&destroyMachine);
    gVtable[1] = reinterpret_cast<void *>(&initMachine);
    gVtable[2] = reinterpret_cast<void *>(&tickMachine);
    gVtable[3] = reinterpret_cast<void *>(&workMachine);
    gVtable[6] = reinterpret_cast<void *>(&saveMachine);
    gVtable[7] = reinterpret_cast<void *>(&attributesChanged);
    gVtable[9] = reinterpret_cast<void *>(&setNumTracks);
    gVtable[12] = reinterpret_cast<void *>(&midiNote);
}

} // namespace

extern "C" __declspec(dllexport) CMachineInfo const *GetInfo() {
    return &kInfo;
}

extern "C" __declspec(dllexport) CMachineInterface *CreateMachine() {
    static bool vtableReady = false;
    if (!vtableReady) {
        initVtable();
        vtableReady = true;
    }
    gMachine = FixtureMachine{};
    gMachine.base.vtable = gVtable;
    gMachine.base.GlobalVals = &gMachine.global;
    return &gMachine.base;
}
