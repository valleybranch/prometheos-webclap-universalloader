#pragma once

#include <string>
#include <vector>

struct ImportResolution {
    std::string name;
    bool resolved = false;
    std::string path;
};

struct ImportProbe {
    std::vector<ImportResolution> imports;
    std::vector<std::string> missing;
};

bool probePeImports(const std::string &modulePath, ImportProbe &out, std::string &error);
std::string importProbeJson(const ImportProbe &probe);
