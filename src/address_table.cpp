#include "address_table.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

std::string ReadValueString(const ProcessManager& pm, uintptr_t addr, ValueType type) {
    if (!pm.IsOpen()) return "??";
    switch (type) {
    case ValueType::Byte: {
        uint8_t v; if (pm.Read(addr, &v, 1)) return std::to_string(v);
        return "??";
    }
    case ValueType::Word: {
        uint16_t v; if (pm.Read(addr, &v, 2)) return std::to_string(v);
        return "??";
    }
    case ValueType::Dword: {
        uint32_t v; if (pm.Read(addr, &v, 4)) return std::to_string(v);
        return "??";
    }
    case ValueType::Qword: {
        uint64_t v; if (pm.Read(addr, &v, 8)) {
            char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); return b;
        }
        return "??";
    }
    case ValueType::Float32: {
        float v; if (pm.Read(addr, &v, 4)) { char b[32]; snprintf(b, sizeof(b), "%g", v); return b; }
        return "??";
    }
    case ValueType::Float64: {
        double v; if (pm.Read(addr, &v, 8)) { char b[32]; snprintf(b, sizeof(b), "%g", v); return b; }
        return "??";
    }
    case ValueType::String: {
        char buf[64] = {}; if (pm.Read(addr, buf, 63)) return std::string(buf);
        return "??";
    }
    case ValueType::AOB: {
        uint8_t buf[8]; if (pm.Read(addr, buf, 8)) {
            std::string r; char h[4];
            for (int i = 0; i < 8; i++) { snprintf(h, sizeof(h), "%02X ", buf[i]); r += h; }
            return r;
        }
        return "??";
    }
    }
    return "??";
}

void AddressTable::Add(uintptr_t addr, ValueType type, const char* desc) {
    AddressEntry e;
    e.address = addr;
    e.type = type;
    if (desc && desc[0]) {
        strncpy(e.description, desc, sizeof(e.description) - 1);
    } else {
        snprintf(e.description, sizeof(e.description), "0x%llX", (unsigned long long)addr);
    }
    m_entries.push_back(e);
}

void AddressTable::Remove(size_t index) {
    if (index < m_entries.size()) {
        m_entries.erase(m_entries.begin() + index);
        if (m_selected >= (int)index) m_selected--;
    }
}

void AddressTable::Clear() {
    m_entries.clear();
    m_selected = -1;
}

bool AddressTable::WriteValueString(const ProcessManager& pm,
    uintptr_t addr, ValueType type, const char* str) const {
    if (!pm.IsOpen()) return false;
    try {
        switch (type) {
        case ValueType::Byte:    { uint8_t v = (uint8_t)std::stoul(str, nullptr, 0); return pm.Write(addr, &v, 1); }
        case ValueType::Word:    { uint16_t v = (uint16_t)std::stoul(str, nullptr, 0); return pm.Write(addr, &v, 2); }
        case ValueType::Dword:   { uint32_t v = (uint32_t)std::stoul(str, nullptr, 0); return pm.Write(addr, &v, 4); }
        case ValueType::Qword:   { uint64_t v = (uint64_t)std::stoull(str, nullptr, 0); return pm.Write(addr, &v, 8); }
        case ValueType::Float32: { float v = std::stof(str); return pm.Write(addr, &v, 4); }
        case ValueType::Float64: { double v = std::stod(str); return pm.Write(addr, &v, 8); }
        case ValueType::String:  { return pm.Write(addr, str, strlen(str) + 1); }
        default: return false;
        }
    } catch (...) { return false; }
}

void AddressTable::UpdateFrozen(const ProcessManager& pm, float dt) {
    m_freezeTimer += dt;
    if (m_freezeTimer < 0.01f) return;
    m_freezeTimer = 0.0f;
    for (auto& e : m_entries) {
        if (e.frozen && e.editValue[0]) {
            WriteValueString(pm, e.address, e.type, e.editValue);
        }
    }
}

void AddressTable::UpdateValues(const ProcessManager& pm) {
    for (auto& e : m_entries) {
        if (!e.isEditing) {
            std::string val = ReadValueString(pm, e.address, e.type);
            strncpy(e.editValue, val.c_str(), sizeof(e.editValue) - 1);
            e.editValue[sizeof(e.editValue) - 1] = '\0';
        }
    }
}

void AddressTable::Save(const char* path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "MWT2\t1\n";
    for (auto& e : m_entries) {
        f << (int)e.type << "\t" << e.address << "\t" << e.frozen << "\t"
          << e.editValue << "\t" << e.description << "\n";
    }
}

void AddressTable::Load(const char* path) {
    std::ifstream f(path);
    if (!f) return;
    m_entries.clear();
    std::string line;
    bool firstLine = true;
    bool newFormat = false;
    while (std::getline(f, line)) {
        if (firstLine) {
            firstLine = false;
            if (line.rfind("MWT2\t", 0) == 0) {
                newFormat = true;
                continue;
            }
        }
        if (newFormat) {
            std::istringstream iss(line);
            std::string typeStr, addrStr, frozenStr, valStr, descStr;
            std::getline(iss, typeStr, '\t');
            std::getline(iss, addrStr, '\t');
            std::getline(iss, frozenStr, '\t');
            std::getline(iss, valStr, '\t');
            std::getline(iss, descStr);
            AddressEntry e = {};
            try {
                e.type = (ValueType)std::stoi(typeStr);
                e.address = (uintptr_t)std::stoull(addrStr);
                e.frozen = (std::stoi(frozenStr) != 0);
            } catch (...) { continue; }
            strncpy(e.editValue, valStr.c_str(), sizeof(e.editValue) - 1);
            strncpy(e.description, descStr.c_str(), sizeof(e.description) - 1);
            m_entries.push_back(e);
        } else {
            std::istringstream iss(line);
            int typeInt, frozen;
            uintptr_t addr;
            if (iss >> typeInt >> addr >> frozen) {
                AddressEntry e = {};
                e.type = (ValueType)typeInt;
                e.address = addr;
                e.frozen = frozen != 0;
                std::string val;
                iss >> val;
                strncpy(e.editValue, val.c_str(), sizeof(e.editValue) - 1);
                std::string desc;
                std::getline(iss >> std::ws, desc);
                strncpy(e.description, desc.c_str(), sizeof(e.description) - 1);
                m_entries.push_back(e);
            }
        }
    }
}
