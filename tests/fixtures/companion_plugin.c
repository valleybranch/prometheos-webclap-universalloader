#include <windows.h>
#include "vst2_abi.h"

#include <stdlib.h>
#include <string.h>

__declspec(dllimport) int companion_value(void);

typedef struct {
    AEffect effect;
} CompanionPlugin;

static CompanionPlugin *pluginOf(AEffect *effect) { return (CompanionPlugin *)effect->object; }

static void VST2_CALL processReplacing(AEffect *effect, float **inputs, float **outputs, int32_t frames) {
    (void)effect; (void)inputs;
    for (int32_t i = 0; i < frames; ++i) {
        outputs[0][i] = 0.0f;
        outputs[1][i] = 0.0f;
    }
}

static intptr_t VST2_CALL dispatcher(AEffect *effect, int32_t opcode, int32_t index,
                                     intptr_t value, void *ptr, float opt) {
    (void)index; (void)value; (void)opt;
    switch (opcode) {
    case effClose:
        free(pluginOf(effect));
        return 1;
    case effGetEffectName:
        strncpy((char *)ptr, "Companion Test", 31);
        ((char *)ptr)[31] = '\0';
        return 1;
    case effGetVendorString:
        strncpy((char *)ptr, "prometheos", 63);
        ((char *)ptr)[63] = '\0';
        return 1;
    case effGetProductString:
        strncpy((char *)ptr, "Companion Test", 63);
        ((char *)ptr)[63] = '\0';
        return 1;
    case effGetVstVersion:
        return 2400;
    default:
        return 0;
    }
}

static void VST2_CALL setParameter(AEffect *effect, int32_t index, float value) {
    (void)effect; (void)index; (void)value;
}

static float VST2_CALL getParameter(AEffect *effect, int32_t index) {
    (void)effect; (void)index;
    return 0.0f;
}

VST2_EXPORT AEffect *VST2_CALL VSTPluginMain(Vst2HostCallback host) {
    if (!host || !host(NULL, audioMasterVersion, 0, 0, NULL, 0.0f)) return NULL;
    CompanionPlugin *plugin = (CompanionPlugin *)calloc(1, sizeof(*plugin));
    if (!plugin) return NULL;
    AEffect *effect = &plugin->effect;
    effect->magic = VST2_MAGIC;
    effect->dispatcher = dispatcher;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->processReplacing = processReplacing;
    effect->numPrograms = 1;
    effect->numParams = 0;
    effect->numInputs = 0;
    effect->numOutputs = 2;
    effect->flags = effFlagsCanReplacing;
    effect->object = plugin;
    effect->uniqueID = 0x43445000 | (companion_value() & 0xff);
    effect->version = 1;
    return effect;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
