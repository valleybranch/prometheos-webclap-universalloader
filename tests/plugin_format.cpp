#include "../host/plugin_format.h"
#include <cassert>

int main() {
    using F = PluginFormat;
    assert(classifyPluginExports(false, false, true, true, true) == F::Buzz);
    assert(classifyPluginExports(false, true, true, true, true) == F::Vst2);
    assert(classifyPluginExports(true, true, true, true, true) == F::Vst3);
    assert(classifyPluginExports(false, false, true, false, false) == F::Vst2);
    assert(classifyPluginExports(false, false, false, false, false) == F::Unsupported);
}
