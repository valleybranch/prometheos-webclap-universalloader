#include "plugin_instance.h"

#include "buzz_instance.h"
#include "pe_imports.h"
#include "vst2_instance.h"
#include "vst3_instance.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

void prepareAudioThread() {
    uint32_t csr = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8040;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

void PluginInstance::warmUp() {
    const int perQuarter = std::max(1, static_cast<int>(std::lround(rate * 0.25 / block_)));
    const int legatoChords = 10;
    const int legatoBlocks = perQuarter * (legatoChords + 2);
    const int detachedChords = 4;
    const int heldBlocks = std::max(1, static_cast<int>(std::lround(rate * 0.2 / block_)));
    const int perDetached = perQuarter * 4;
    const int blocks = legatoBlocks + perDetached * detachedChords;
    std::vector<float> in(std::max(1, inPorts_) * 2 * block_), out(outPorts_ * 2 * block_);
    vstb_request request{};
    request.frames = block_;
    request.tempo = 120.0;
    request.connectedMask = 0xffffffffu;
    const int voicing[8] = {0, 7, 12, 16, 19, 24, 28, 31};
    auto chordNote = [&](int chord, int v) { return static_cast<uint8_t>(24 + (chord * 7) % 48 + voicing[v]); };
    auto velocity = [&](int chord, int v) { return static_cast<uint8_t>(40 + (chord * 29 + v * 11) % 88); };
    for (int b = 0; b < blocks; ++b) {
        request.blockIndex = b;
        request.eventCount = 0;
        auto add = [&](uint8_t status, uint8_t data1, uint8_t data2) {
            vstb_event &ev = request.events[request.eventCount++];
            ev = vstb_event{};
            ev.type = VSTB_EV_MIDI;
            ev.offset = static_cast<uint32_t>(request.eventCount * 7 % block_);
            ev.midi[0] = status;
            ev.midi[1] = data1;
            ev.midi[2] = data2;
        };
        if (b < legatoBlocks) {
            if (b % perQuarter == 0) {
                const int chord = b / perQuarter;
                if (chord > 0 && chord <= legatoChords)
                    for (int v = 0; v < 8; ++v) add(0x80, chordNote(chord - 1, v), 0);
                if (chord < legatoChords)
                    for (int v = 0; v < 8; ++v) add(0x90, chordNote(chord, v), velocity(chord, v));
            }
            if (b == legatoBlocks - 1) add(0xB0, 123, 0);
        } else {
            const int d = b - legatoBlocks;
            const int chord = legatoChords + d / perDetached;
            request.flags = VSTB_REQ_PLAYING;
            request.samplePos = static_cast<double>(d) * block_;
            request.ppqPos = request.samplePos / rate * (request.tempo / 60.0);
            if (d % perDetached == 0)
                for (int v = 0; v < 8; ++v) add(0x90, chordNote(chord, v), velocity(chord, v));
            if (d % perDetached == heldBlocks)
                for (int v = 0; v < 8; ++v) add(0x80, chordNote(chord, v), 0);
        }
        for (int f = 0; f < block_; ++f) {
            const double t = static_cast<double>(b * block_ + f) / rate;
            const float s = static_cast<float>(0.2 * std::exp(-3.0 * std::fmod(t, 1.0)) * (2.0 * std::fmod(t * 110.0, 1.0) - 1.0));
            for (int c = 0; c < inPorts_ * 2; ++c) in[c * block_ + f] = s;
        }
        process(request, in.data(), out.data());
    }
    reset();
}

std::unique_ptr<PluginInstance> loadPlugin(const std::string &path, double rate, int block, std::string &error) {
    HMODULE dll = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        const DWORD code = GetLastError();
        if (code == ERROR_MOD_NOT_FOUND) {
            ImportProbe probe;
            std::string probeError;
            if (probePeImports(path, probe, probeError)) {
                if (!probe.missing.empty()) {
                    error = "missing-direct-dependency:";
                    for (size_t i = 0; i < probe.missing.size(); ++i) {
                        if (i) error += ",";
                        error += probe.missing[i];
                    }
                } else {
                    error = "loader-dependency-failure:error 126; direct imports=";
                    for (size_t i = 0; i < probe.imports.size(); ++i) {
                        if (i) error += ",";
                        error += probe.imports[i].name;
                    }
                }
            } else {
                error = "loader-dependency-failure:error 126; import probe failed: " + probeError;
            }
        } else {
            error = "LoadLibraryEx failed, error " + std::to_string(code);
        }
        return nullptr;
    }
    std::unique_ptr<PluginInstance> plugin;
    if (GetProcAddress(dll, "GetPluginFactory")) plugin = std::make_unique<Vst3Instance>();
    else if (GetProcAddress(dll, "VSTPluginMain") || GetProcAddress(dll, "main")) plugin = std::make_unique<Vst2Instance>();
    else if (GetProcAddress(dll, "GetInfo") && GetProcAddress(dll, "CreateMachine")) plugin = std::make_unique<BuzzMachineInstance>();
    else {
        error = "unsupported Windows plugin: expected VST2, VST3 or Buzz GetInfo/CreateMachine exports";
        FreeLibrary(dll);
        return nullptr;
    }
    FreeLibrary(dll);
    if (!plugin->load(path, rate, block, error)) return nullptr;
    return plugin;
}
