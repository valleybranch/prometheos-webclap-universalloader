#pragma once
#include "plugin_instance.h"
#include "buzz_abi.h"
#include <windows.h>
#ifdef MessageBox
#undef MessageBox
#endif
#include <string>
#include <vector>

class BuzzMachineInstance final : public PluginInstance {
public:
    ~BuzzMachineInstance() override;
    bool load(const std::string &path, double rate, int block, std::string &error) override;
    void close() override;
    void process(const vstb_request &request, const float *inputs, float *outputs) override;
    std::string describeJson() override;
    std::vector<uint8_t> getState() override;
    bool setState(const uint8_t *data, size_t size, std::string &error) override;
    int latency() const override { return 0; }

protected:
    void reset() override;

private:
    using GetInfoFn = CMachineInfo const *(*)();
    using CreateMachineFn = CMachineInterface *(*)();

    struct BuzzCallbacks final : CMICallbacks {
        float aux[256 * 2]{};
        std::string lastMessage;
        float *GetAuxBuffer() override { return aux; }
        void ClearAuxBuffer() override;
        void MessageBox(char const *text) override;
    };

    HMODULE module_ = nullptr;
    GetInfoFn getInfo_ = nullptr;
    CreateMachineFn createMachine_ = nullptr;
    const CMachineInfo *info_ = nullptr;
    CMachineInterface *machine_ = nullptr;
    BuzzCallbacks callbacks_;
    CMasterInfo master_{};
    int tracks_ = 0;
    std::vector<uint8_t> globals_;
    std::vector<uint8_t> trackVals_;
    std::vector<int> attrs_;
    std::vector<float> mono_;

    bool create(std::string &error);
    void destroy();
    void initialise(const uint8_t *data, size_t size);
    static int paramSize(const CMachineParameter &p);
    void clearNoValues();
};
