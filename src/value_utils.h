#pragma once
#include "types.h"
#include "process_manager.h"
#include <string>

inline std::string ReadValueString(const ProcessManager& pm, uintptr_t addr, ValueType type, size_t aobLen = 8) {
    if (!pm.IsOpen()) return "??";
    switch (type) {
    case ValueType::Byte: { uint8_t v; if (pm.Read(addr, &v, 1)) return std::to_string(v); return "??"; }
    case ValueType::Word: { uint16_t v; if (pm.Read(addr, &v, 2)) return std::to_string(v); return "??"; }
    case ValueType::Dword: { uint32_t v; if (pm.Read(addr, &v, 4)) return std::to_string(v); return "??"; }
    case ValueType::Qword: { uint64_t v; if (pm.Read(addr, &v, 8)) { char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); return b; } return "??"; }
    case ValueType::Float32: { float v; if (pm.Read(addr, &v, 4)) { char b[32]; snprintf(b, sizeof(b), "%g", v); return b; } return "??"; }
    case ValueType::Float64: { double v; if (pm.Read(addr, &v, 8)) { char b[32]; snprintf(b, sizeof(b), "%g", v); return b; } return "??"; }
    case ValueType::String: { char buf[64] = {}; if (pm.Read(addr, buf, 63)) return std::string(buf); return "??"; }
    case ValueType::AOB: {
        if (aobLen == 0 || aobLen > 64) aobLen = 8;
        uint8_t buf[64]; if (pm.Read(addr, buf, aobLen)) {
            std::string r; char h[4];
            for (size_t i = 0; i < aobLen; i++) { snprintf(h, sizeof(h), "%02X ", buf[i]); r += h; }
            return r;
        }
        return "??";
    }
    }
    return "??";
}

inline bool WriteValueString(const ProcessManager& pm, uintptr_t addr, ValueType type, const char* str) {
    if (!pm.IsOpen()) return false;
    try {
        switch (type) {
        case ValueType::Byte: { uint8_t v = (uint8_t)std::stoul(str, nullptr, 0); return pm.Write(addr, &v, 1); }
        case ValueType::Word: { uint16_t v = (uint16_t)std::stoul(str, nullptr, 0); return pm.Write(addr, &v, 2); }
        case ValueType::Dword: { uint32_t v = (uint32_t)std::stoul(str, nullptr, 0); return pm.Write(addr, &v, 4); }
        case ValueType::Qword: { uint64_t v = (uint64_t)std::stoull(str, nullptr, 0); return pm.Write(addr, &v, 8); }
        case ValueType::Float32: { float v = std::stof(str); return pm.Write(addr, &v, 4); }
        case ValueType::Float64: { double v = std::stod(str); return pm.Write(addr, &v, 8); }
        case ValueType::String: { return pm.Write(addr, str, strlen(str) + 1); }
        default: return false;
        }
    } catch (...) { return false; }
}
