#pragma once
#include "types.h"
#include "process_manager.h"
#include <vector>

class AddressTable {
public:
    void Add(uintptr_t addr, ValueType type, const char* desc = "");
    void Remove(size_t index);
    void Clear();

    void Render(const ProcessManager& pm);
    void UpdateFrozen(const ProcessManager& pm, float dt);
    void UpdateValues(const ProcessManager& pm);

    void Save(const char* path) const;
    void Load(const char* path);

    size_t Count() const { return m_entries.size(); }

private:
    std::vector<AddressEntry> m_entries;
    float m_freezeTimer = 0.0f;
    float m_valueTimer = 0.0f;

    void WriteValue(const ProcessManager& pm, size_t index);
    std::string ReadValueString(const ProcessManager& pm, uintptr_t addr, ValueType type) const;
    bool WriteValueString(const ProcessManager& pm, uintptr_t addr, ValueType type, const char* str) const;
};
