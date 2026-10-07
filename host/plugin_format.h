#pragma once

enum class PluginFormat {
    Unsupported,
    Vst2,
    Vst3,
    Buzz,
};

inline PluginFormat classifyPluginExports(bool hasFactory,
                                          bool hasVstPluginMain,
                                          bool hasLegacyMain,
                                          bool hasBuzzGetInfo,
                                          bool hasBuzzCreateMachine) {
    if (hasFactory) return PluginFormat::Vst3;
    if (hasVstPluginMain) return PluginFormat::Vst2;
    if (hasBuzzGetInfo && hasBuzzCreateMachine) return PluginFormat::Buzz;
    if (hasLegacyMain) return PluginFormat::Vst2;
    return PluginFormat::Unsupported;
}
