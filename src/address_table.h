#pragma once
#include "types.h"
#include "process_manager.h"
#include <algorithm>
#include <vector>
#include <string>

class AddressTable {
public:
    std::vector<AddressEntry>& Entries() { return m_entries; }

    void Add(uintptr_t addr, ValueType type, const char* desc = "");
    void Remove(size_t index);
    void Clear();

    void UpdateFrozen(const ProcessManager& pm, float dt);
    void UpdateValues(const ProcessManager& pm);

    void Save(const char* path) const;
    void Load(const char* path);

    size_t Count() const { return m_entries.size(); }

    // Returns index of selected entry, or -1
    int GetSelected() const { return m_selected; }
    void SetSelected(int idx) { m_selected = idx; }

    int GetScrollPos() const { return m_scrollPos; }
    void SetScrollPos(int pos) { m_scrollPos = pos; }
    void ClampScroll(int maxScroll) { m_scrollPos = std::max(0, std::min(m_scrollPos, maxScroll)); }

private:
    std::vector<AddressEntry> m_entries;
    float m_freezeTimer = 0.0f;
    int m_selected = -1;
    int m_scrollPos = 0;
};
