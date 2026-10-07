// One loaded plugin, driven block by block from vstbridge requests, whatever
// its format (VST2: vst2_instance.h, VST3: vst3_instance.h).
//
// The real-time bridge (vsthost --bridge) and the offline replay
// (vsthost --replay) both run plugins through this interface on a thread set
// up by prepareAudioThread(), so a recorded request stream replayed offline
// renders exactly what the bridge streamed.
#pragma once

#include "vstbridge_abi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// FTZ/DAZ in MXCSR, so denormals cannot stall the emulated FPU, and a
// time-critical priority. (The x87 unit has no flush-to-zero; its control word
// keeps the thread default.)
void prepareAudioThread();

class PluginInstance {
  public:
    virtual ~PluginInstance() = default;

    virtual bool load(const std::string &path, double rate, int block, std::string &error) = 0;
    virtual void close() = 0;

    // One block: events from `request`, inputs planar (inPorts x 2 x block),
    // outputs planar (outPorts x 2 x block). No allocation.
    virtual void process(const vstb_request &request, const float *inputs, float *outputs) = 0;

    // The same description for every format (the wrapper reads it): format,
    // name, product, vendor, vendorVersion, synth, inPorts, outPorts, latency,
    // programs and params ({name, label, display, value} with values 0..1;
    // VSTB_EV_PARAM indexes this list).
    virtual std::string describeJson() = 0;
    virtual std::vector<uint8_t> getState() = 0;
    virtual bool setState(const uint8_t *data, size_t size, std::string &error) = 0;
    virtual int latency() const = 0;

    // Renders a few seconds of chords and their release, then resets the
    // plugin, so live notes don't pay first-time JIT translation of the note path.
    void warmUp();

    int block() const { return block_; }
    int inPorts() const { return inPorts_; }
    int outPorts() const { return outPorts_; }
    bool takeIoChanged() {
        const bool changed = ioChanged;
        ioChanged = false;
        return changed;
    }

    double rate = 44100.0;
    // Set when the plugin reports new I/O or latency.
    bool ioChanged = false;

  protected:
    // Back to a fresh processing state (after the warm-up), keeping parameters.
    virtual void reset() = 0;

    int block_ = 256;
    int inPorts_ = 0;
    int outPorts_ = 1;
};

// Classifies a Windows loader failure using the PE import probe. Direct missing
// imports are distinguished from transitive/other loader dependency failures.
std::string classifyPluginLoadFailure(const std::string &path, unsigned long code);

// Loads `path` as whichever format it exports (GetPluginFactory: VST3,
// VSTPluginMain/main: VST2).
std::unique_ptr<PluginInstance> loadPlugin(const std::string &path, double rate, int block, std::string &error);
