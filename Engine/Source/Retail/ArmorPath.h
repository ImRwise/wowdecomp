#ifndef ENGINE_SOURCE_RETAIL_ARMORPATH_H
#define ENGINE_SOURCE_RETAIL_ARMORPATH_H

// The stock component code builds item file names as <folder> + ItemDisplayInfo name (+ ".blp").
// ServerSync writes complete virtual paths ("retail/<FileDataID>.m2" / ".blp") into those columns;
// SFile already routes any such path to the Retail source (packed EsteriaRetail\ -> decoded cache ->
// CASC/CDN), so the only thing in the way is the folder prefix and the doubled extension.
// These helpers keep that decision in one tested place.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace Retail::Armor {

// True for "retail/<digits>.<ext>" in either slash style and any case.
inline bool ParseRetailName(const char* name, uint32_t* fileDataId = nullptr, std::string* extension = nullptr) {
    if (!name) return false;
    static const char prefix[] = "retail/";
    size_t i = 0;
    for (; prefix[i]; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c == '\\') c = '/';
        if (c != prefix[i]) return false;
    }
    uint64_t id = 0;
    size_t digits = 0;
    for (; name[i] >= '0' && name[i] <= '9'; ++i, ++digits) {
        id = id * 10 + uint64_t(name[i] - '0');
        if (id > 0xFFFFFFFFull) return false;
    }
    if (!digits || name[i] != '.' || !name[i + 1]) return false;
    if (fileDataId) *fileDataId = uint32_t(id);
    if (extension) *extension = name + i;
    return true;
}

inline bool IsRetailName(const char* name) { return ParseRetailName(name); }

inline std::string FileName(uint32_t fileDataId, const char* extension) {
    return "retail/" + std::to_string(fileDataId) + extension;
}

// Folder + model name, as the stock code does, except that a Retail name is already complete.
inline std::string ModelPath(const char* folder, const char* name) {
    if (!name) return {};
    if (IsRetailName(name)) return name;
    return std::string(folder ? folder : "") + name;
}

// Folder + texture name + ".blp" (or `suffix`), except that a Retail name is complete.
inline std::string TexturePath(const char* folder, const char* name, const char* suffix = ".blp") {
    if (!name) return {};
    if (IsRetailName(name)) return name;
    return std::string(folder ? folder : "") + name + suffix;
}

// Copy into a fixed buffer without overrunning it. Returns false (and leaves a terminated empty
// string) when the path does not fit, so a hostile or corrupt DBC row cannot overflow s_buffer.
inline bool CopyPath(char* out, size_t size, const std::string& path) {
    if (!out || !size) return false;
    if (path.size() + 1 > size) { out[0] = '\0'; return false; }
    std::memcpy(out, path.c_str(), path.size() + 1);
    return true;
}

}
#endif
