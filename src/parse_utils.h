#pragma once
#include "types.h"
#include <string>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cstring>

// ============================================================
// Pure, process-independent parsing helpers.
//
// These are factored out of Scanner::ParseAndWrite and
// ProcessManager::ParseAddressString so the parsing edge cases can be
// exercised by the unit test target without a live process.
// ============================================================

// Parse a numeric string into 'size' raw little-endian bytes at 'out' per the
// given ValueType. Returns false on parse failure (non-numeric, out of range).
// Integer values wider than the target type are truncated to the low bytes,
// matching the historical C-style cast behavior (e.g. Byte "256" -> 0).
inline bool ParseValueToBytes(ValueType type, const std::string& str,
                              uint8_t* out, size_t /*size*/, bool hex) {
    int base = hex ? 16 : 10;
    try {
        switch (type) {
        case ValueType::Byte: {
            auto v = (uint8_t)std::stoul(str, nullptr, base);
            memcpy(out, &v, 1); return true;
        }
        case ValueType::Word: {
            auto v = (uint16_t)std::stoul(str, nullptr, base);
            memcpy(out, &v, 2); return true;
        }
        case ValueType::Dword: {
            auto v = (uint32_t)std::stoul(str, nullptr, base);
            memcpy(out, &v, 4); return true;
        }
        case ValueType::Qword: {
            auto v = (uint64_t)std::stoull(str, nullptr, base);
            memcpy(out, &v, 8); return true;
        }
        case ValueType::Float32: {
            float v = std::stof(str);
            memcpy(out, &v, 4); return true;
        }
        case ValueType::Float64: {
            double v = std::stod(str);
            memcpy(out, &v, 8); return true;
        }
        default: return false;
        }
    } catch (...) { return false; }
}

// Split a "module+offset" address string into a trimmed module name and the
// parsed offset. Returns true when the string is in module-relative form
// (contains '+'); false for a plain address (caller should parse as hex).
inline bool SplitModuleOffset(const std::string& str,
                              std::string& moduleOut, uintptr_t& offsetOut) {
    size_t plus = str.find('+');
    if (plus == std::string::npos) return false;

    std::string modName = str.substr(0, plus);
    std::string offsetStr = str.substr(plus + 1);

    // Trim trailing whitespace from module name
    while (!modName.empty() && isspace((unsigned char)modName.back()))
        modName.pop_back();
    // Trim leading whitespace from offset
    size_t start = offsetStr.find_first_not_of(" \t");
    if (start != std::string::npos) offsetStr = offsetStr.substr(start);

    moduleOut = modName;
    // Cheat-Engine convention: module offsets are hexadecimal. Base 16 also
    // accepts a leading "0x" prefix, so it handles both bare and prefixed forms.
    offsetOut = (uintptr_t)strtoull(offsetStr.c_str(), nullptr, 16);
    return true;
}

// Parse a plain hexadecimal address string (optional "0x" prefix accepted).
inline uintptr_t ParsePlainHexAddress(const std::string& str) {
    return (uintptr_t)strtoull(str.c_str(), nullptr, 16);
}
