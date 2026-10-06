#include "pe_imports.h"

#include "json.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

bool readAll(const std::string &path, std::vector<unsigned char> &data, std::string &error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        error = "cannot stat " + path;
        return false;
    }
    data.resize(static_cast<size_t>(size));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        error = "cannot read " + path;
        return false;
    }
    std::fclose(f);
    return true;
}

template <typename T>
bool readAt(const std::vector<unsigned char> &data, size_t offset, T &out) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string moduleDir(const std::string &path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

bool fileExists(const std::string &path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string searchPath(const std::string &name) {
    char buffer[MAX_PATH * 4] = {};
    const DWORD n = SearchPathA(nullptr, name.c_str(), nullptr, static_cast<DWORD>(sizeof buffer), buffer, nullptr);
    return n && n < sizeof buffer ? std::string(buffer, n) : std::string();
}

bool rvaToOffset(DWORD rva, DWORD bytes, const IMAGE_NT_HEADERS32 &nt,
                 const std::vector<IMAGE_SECTION_HEADER> &sections, size_t fileSize,
                 size_t &offset, std::string &error) {
    if (rva < nt.OptionalHeader.SizeOfHeaders) {
        if (static_cast<unsigned long long>(rva) + bytes > fileSize) {
            error = "truncated PE header RVA";
            return false;
        }
        offset = rva;
        return true;
    }
    for (const auto &s : sections) {
        const DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
        if (rva < s.VirtualAddress || rva - s.VirtualAddress >= span) continue;
        const unsigned long long raw = static_cast<unsigned long long>(s.PointerToRawData) + (rva - s.VirtualAddress);
        if (raw + bytes > fileSize || rva - s.VirtualAddress + bytes > s.SizeOfRawData) {
            error = "truncated PE section data";
            return false;
        }
        offset = static_cast<size_t>(raw);
        return true;
    }
    error = "PE RVA outside sections";
    return false;
}

bool readCStringAtRva(const std::vector<unsigned char> &data, DWORD rva, const IMAGE_NT_HEADERS32 &nt,
                      const std::vector<IMAGE_SECTION_HEADER> &sections, std::string &out, std::string &error) {
    size_t off = 0;
    if (!rvaToOffset(rva, 1, nt, sections, data.size(), off, error)) return false;
    size_t end = off;
    while (end < data.size() && data[end] != 0) ++end;
    if (end == data.size()) {
        error = "unterminated PE import name";
        return false;
    }
    out.assign(reinterpret_cast<const char *>(data.data() + off), end - off);
    return true;
}

} // namespace

bool probePeImports(const std::string &modulePath, ImportProbe &out, std::string &error) {
    out = ImportProbe{};
    error.clear();

    std::vector<unsigned char> data;
    if (!readAll(modulePath, data, error)) return false;

    IMAGE_DOS_HEADER dos{};
    if (!readAt(data, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        error = "unsupported binary: invalid DOS header";
        return false;
    }

    const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS32 nt{};
    if (!readAt(data, ntOffset, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        error = "unsupported binary: expected x86 PE32";
        return false;
    }

    const size_t sectionOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    for (size_t i = 0; i < sections.size(); ++i) {
        if (!readAt(data, sectionOffset + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) {
            error = "truncated PE section table";
            return false;
        }
    }

    const auto &dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return true;

    std::map<std::string, ImportResolution> found;
    const std::string baseDir = moduleDir(modulePath);
    for (size_t index = 0;; ++index) {
        size_t off = 0;
        const unsigned long long rva64 = static_cast<unsigned long long>(dir.VirtualAddress) +
                                         index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (rva64 > 0xffffffffULL ||
            !rvaToOffset(static_cast<DWORD>(rva64), sizeof(IMAGE_IMPORT_DESCRIPTOR), nt, sections, data.size(), off, error))
            return false;
        IMAGE_IMPORT_DESCRIPTOR desc{};
        if (!readAt(data, off, desc)) {
            error = "truncated PE import table";
            return false;
        }
        if (!desc.OriginalFirstThunk && !desc.FirstThunk && !desc.Name) break;
        if (!desc.Name) {
            error = "malformed PE import descriptor";
            return false;
        }

        std::string name;
        if (!readCStringAtRva(data, desc.Name, nt, sections, name, error)) return false;
        const size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos || name.empty()) {
            error = "malformed PE import name";
            return false;
        }

        ImportResolution item;
        item.name = name;
        const std::string beside = baseDir + "\\" + name;
        if (fileExists(beside)) {
            item.resolved = true;
            item.path = beside;
        } else {
            item.path = searchPath(name);
            item.resolved = !item.path.empty();
        }
        const std::string key = lower(name);
        if (!found.count(key)) found.emplace(key, item);
    }

    for (const auto &entry : found) {
        out.imports.push_back(entry.second);
        if (!entry.second.resolved) out.missing.push_back(entry.second.name);
    }
    return true;
}

std::string importProbeJson(const ImportProbe &probe) {
    std::string json = "{\"imports\":[";
    for (size_t i = 0; i < probe.imports.size(); ++i) {
        const auto &item = probe.imports[i];
        if (i) json += ",";
        json += "{\"name\":\"" + jsonEscape(item.name) + "\",\"resolved\":" +
                std::string(item.resolved ? "true" : "false") + ",\"path\":\"" +
                jsonEscape(item.path) + "\"}";
    }
    json += "],\"missing\":[";
    for (size_t i = 0; i < probe.missing.size(); ++i) {
        if (i) json += ",";
        json += "\"" + jsonEscape(probe.missing[i]) + "\"";
    }
    return json + "]}";
}
