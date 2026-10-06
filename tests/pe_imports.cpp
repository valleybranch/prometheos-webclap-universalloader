#include "../host/pe_imports.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const char *name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static bool contains(const std::vector<std::string> &items, const char *name) {
    for (const auto &item : items) if (_stricmp(item.c_str(), name) == 0) return true;
    return false;
}

int main(int argc, char **argv) {
    ImportProbe probe;
    std::string error;

    if (argc == 3 && std::strcmp(argv[1], "--json") == 0) {
        if (!probePeImports(argv[2], probe, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        std::puts(importProbeJson(probe).c_str());
        return 0;
    }

    DeleteFileA("build\\COMPANION_DEP.DLL");
    check(probePeImports("build\\companion_plugin.dll", probe, error) &&
          contains(probe.missing, "COMPANION_DEP.DLL"),
          "reports_exact_missing_companion");

    CopyFileA("build\\companion_dep.dll", "build\\COMPANION_DEP.DLL", FALSE);
    probe = {};
    error.clear();
    check(probePeImports("build\\companion_plugin.dll", probe, error) &&
          !contains(probe.missing, "COMPANION_DEP.DLL"),
          "resolves_companion_beside_primary");

    bool windowsOk = false;
    for (const auto &item : probe.imports)
        if (_stricmp(item.name.c_str(), "KERNEL32.DLL") == 0 || _stricmp(item.name.c_str(), "msvcrt.dll") == 0)
            windowsOk = windowsOk || item.resolved;
    check(windowsOk, "resolves_normal_windows_imports");

    FILE *src = std::fopen("build\\companion_plugin.dll", "rb");
    std::vector<unsigned char> bytes;
    if (src) {
        std::fseek(src, 0, SEEK_END);
        const long n = std::ftell(src);
        std::fseek(src, 0, SEEK_SET);
        if (n > 0) {
            bytes.resize(static_cast<size_t>(n));
            std::fread(bytes.data(), 1, bytes.size(), src);
        }
        std::fclose(src);
    }
    if (bytes.size() > 512) bytes.resize(bytes.size() / 2);
    FILE *dst = std::fopen("build\\companion_plugin_truncated.dll", "wb");
    if (dst) {
        std::fwrite(bytes.data(), 1, bytes.size(), dst);
        std::fclose(dst);
    }
    probe = {};
    error.clear();
    check(!probePeImports("build\\companion_plugin_truncated.dll", probe, error),
          "rejects_truncated_import_table");

    DeleteFileA("build\\COMPANION_DEP.DLL");
    return failures ? 1 : 0;
}
