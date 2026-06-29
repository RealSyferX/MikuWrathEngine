#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

enum class ValueType : int {
    Byte = 0,
    Word,
    Dword,
    Qword,
    Float32,
    Float64,
    String,
    AOB
};

inline const char* ValueTypeName(ValueType t) {
    switch (t) {
    case ValueType::Byte:    return "Byte";
    case ValueType::Word:    return "2 Bytes";
    case ValueType::Dword:   return "4 Bytes";
    case ValueType::Qword:   return "8 Bytes";
    case ValueType::Float32: return "Float";
    case ValueType::Float64: return "Double";
    case ValueType::String:  return "String";
    case ValueType::AOB:     return "AOB";
    }
    return "?";
}

inline size_t ValueTypeSize(ValueType t) {
    switch (t) {
    case ValueType::Byte:    return 1;
    case ValueType::Word:    return 2;
    case ValueType::Dword:   return 4;
    case ValueType::Qword:   return 8;
    case ValueType::Float32: return 4;
    case ValueType::Float64: return 8;
    default: return 0;
    }
}

inline bool IsNumericType(ValueType t) {
    return t <= ValueType::Float64;
}

struct ProcessInfo {
    DWORD pid = 0;
    std::string name;
};

struct MemoryRegion {
    uintptr_t base = 0;
    size_t size = 0;
    DWORD protect = 0;
    bool writable = false;
};

struct AddressEntry {
    uintptr_t address = 0;
    ValueType type = ValueType::Dword;
    char description[128] = {};
    bool frozen = false;
    char editValue[64] = {};
    bool isEditing = false;
};

struct DisasmInstruction {
    uint64_t address = 0;
    uint8_t bytes[16] = {};
    uint8_t size = 0;
    char mnemonic[32] = {};
    char opStr[160] = {};
};
