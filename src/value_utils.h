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
    case ValueType::String: {
        char buf[64] = {};
        size_t got = pm.ReadPartial(addr, buf, 63);
        if (got == 0) return "??";
        if (got > 63) got = 63;
        buf[got] = '\0';
        return std::string(buf);
    }
    case ValueType::AOB: {
        if (aobLen == 0 || aobLen > 64) aobLen = 8;
        uint8_t buf[64];
        size_t got = pm.ReadPartial(addr, buf, aobLen);
        if (got == 0) return "??";
        std::string r; char h[4];
        for (size_t i = 0; i < got; i++) { snprintf(h, sizeof(h), "%02X ", buf[i]); r += h; }
        for (size_t i = got; i < aobLen; i++) r += "?? ";
        return r;
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
        case ValueType::AOB: {
            // Parse space-separated hex tokens (matching ReadValueString's "%02X " format).
            // "??"/"?" tokens are wildcards that leave the existing byte untouched.
            std::vector<uint8_t> bytes;
            std::vector<bool> wildcard;
            bool anyWildcard = false;
            const char* p = str;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                const char* start = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                std::string tok(start, p - start);
                if (tok == "??" || tok == "?") {
                    bytes.push_back(0);
                    wildcard.push_back(true);
                    anyWildcard = true;
                } else {
                    size_t consumed = 0;
                    unsigned long v = std::stoul(tok, &consumed, 16);
                    if (consumed != tok.size() || v > 0xFF) return false;
                    bytes.push_back((uint8_t)v);
                    wildcard.push_back(false);
                }
            }
            if (bytes.empty()) return false;
            if (!anyWildcard) return pm.Write(addr, bytes.data(), bytes.size());
            // Overlay only non-wildcard positions onto the current bytes.
            std::vector<uint8_t> cur(bytes.size());
            if (!pm.Read(addr, cur.data(), cur.size())) return false;
            for (size_t i = 0; i < bytes.size(); i++)
                if (!wildcard[i]) cur[i] = bytes[i];
            return pm.Write(addr, cur.data(), cur.size());
        }
        default: return false;
        }
    } catch (...) { return false; }
}
