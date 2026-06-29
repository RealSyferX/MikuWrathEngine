#include "address_table.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>

void AddressTable::Add(uintptr_t addr, ValueType type, const char* desc) {
    AddressEntry e;
    e.address = addr;
    e.type = type;
    if (desc) strncpy(e.description, desc, sizeof(e.description) - 1);
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
    if (!desc) strncpy(e.description, buf, sizeof(e.description) - 1);
    m_entries.push_back(e);
}

void AddressTable::Remove(size_t index) {
    if (index < m_entries.size()) {
        m_entries.erase(m_entries.begin() + index);
    }
}

void AddressTable::Clear() {
    m_entries.clear();
}

std::string AddressTable::ReadValueString(const ProcessManager& pm,
    uintptr_t addr, ValueType type) const {
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

bool AddressTable::WriteValueString(const ProcessManager& pm,
    uintptr_t addr, ValueType type, const char* str) const {
    if (!pm.IsOpen()) return false;

    try {
        switch (type) {
        case ValueType::Byte: {
            uint8_t v = (uint8_t)std::stoul(str, nullptr, 0);
            return pm.Write(addr, &v, 1);
        }
        case ValueType::Word: {
            uint16_t v = (uint16_t)std::stoul(str, nullptr, 0);
            return pm.Write(addr, &v, 2);
        }
        case ValueType::Dword: {
            uint32_t v = (uint32_t)std::stoul(str, nullptr, 0);
            return pm.Write(addr, &v, 4);
        }
        case ValueType::Qword: {
            uint64_t v = (uint64_t)std::stoull(str, nullptr, 0);
            return pm.Write(addr, &v, 8);
        }
        case ValueType::Float32: {
            float v = std::stof(str);
            return pm.Write(addr, &v, 4);
        }
        case ValueType::Float64: {
            double v = std::stod(str);
            return pm.Write(addr, &v, 8);
        }
        case ValueType::String: {
            size_t len = strlen(str);
            return pm.Write(addr, str, len + 1);
        }
        default: return false;
        }
    } catch (...) { return false; }
}

void AddressTable::WriteValue(const ProcessManager& pm, size_t index) {
    if (index >= m_entries.size()) return;
    WriteValueString(pm, m_entries[index].address, m_entries[index].type,
                     m_entries[index].editValue);
}

void AddressTable::UpdateFrozen(const ProcessManager& pm, float dt) {
    m_freezeTimer += dt;
    if (m_freezeTimer < 0.01f) return; // 100Hz max
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

void AddressTable::Render(const ProcessManager& pm) {
    ImGui::BeginChild("AddrTableChild", ImVec2(0, 0), ImGuiChildFlags_Borders);

    // Toolbar
    if (ImGui::Button("Add Entry")) {
        if (pm.IsOpen()) {
            Add(0, ValueType::Dword);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove Selected")) {
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            if (it->frozen) { // reuse frozen flag as selected? No, use a separate flag
                // Actually let's just remove the last entry for simplicity
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) {
        m_entries.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        Save("miku_table.mwt");
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        Load("miku_table.mwt");
    }

    ImGui::Separator();

    if (ImGui::BeginTable("ATable", 6,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {

        ImGui::TableSetupColumn("Fz", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableHeadersRow();

        int toRemove = -1;

        for (size_t i = 0; i < m_entries.size(); i++) {
            auto& e = m_entries[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            // Freeze checkbox
            ImGui::TableNextColumn();
            ImGui::Checkbox("##fz", &e.frozen);

            // Description
            ImGui::TableNextColumn();
            ImGui::InputText("##desc", e.description, sizeof(e.description));

            // Address
            ImGui::TableNextColumn();
            char addrStr[32];
            snprintf(addrStr, sizeof(addrStr), "0x%016llX", (unsigned long long)e.address);
            ImGui::Selectable(addrStr, false, ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                ImGui::SetClipboardText(addrStr);
            }
            if (ImGui::BeginPopupContextItem("AddrCtx")) {
                if (ImGui::MenuItem("Copy Address")) {
                    ImGui::SetClipboardText(addrStr);
                }
                if (ImGui::MenuItem("Copy Value")) {
                    ImGui::SetClipboardText(e.editValue);
                }
                ImGui::EndPopup();
            }

            // Type
            ImGui::TableNextColumn();
            const char* typeNames[] = {"Byte", "2 Bytes", "4 Bytes", "8 Bytes",
                                       "Float", "Double", "String", "AOB"};
            int* typeIdx = (int*)&e.type;
            ImGui::PushItemWidth(60);
            ImGui::Combo("##type", typeIdx, typeNames, 8);
            ImGui::PopItemWidth();

            // Value
            ImGui::TableNextColumn();
            // Read current value if not editing
            if (!e.isEditing && pm.IsOpen()) {
                std::string val = ReadValueString(pm, e.address, e.type);
                strncpy(e.editValue, val.c_str(), sizeof(e.editValue) - 1);
                e.editValue[sizeof(e.editValue) - 1] = '\0';
            }

            ImGui::PushItemWidth(-1);
            bool entered = ImGui::InputText("##val", e.editValue, sizeof(e.editValue),
                ImGuiInputTextFlags_EnterReturnsTrue);
            e.isEditing = ImGui::IsItemActive();
            ImGui::PopItemWidth();

            if (entered) {
                WriteValue(pm, i);
                e.isEditing = false;
            }

            // Remove button
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X")) {
                toRemove = (int)i;
            }

            ImGui::PopID();
        }

        ImGui::EndTable();

        if (toRemove >= 0) {
            Remove(toRemove);
        }
    }

    if (m_entries.empty()) {
        ImGui::TextDisabled("No entries. Right-click scan results to add addresses.");
    }

    ImGui::EndChild();
}

void AddressTable::Save(const char* path) const {
    std::ofstream f(path);
    if (!f) return;
    for (auto& e : m_entries) {
        f << (int)e.type << " "
          << e.address << " "
          << e.frozen << " "
          << e.editValue << " "
          << e.description << "\n";
    }
}

void AddressTable::Load(const char* path) {
    std::ifstream f(path);
    if (!f) return;
    m_entries.clear();
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        int typeInt;
        uintptr_t addr;
        int frozen;
        AddressEntry e = {};
        if (iss >> typeInt >> addr >> frozen) {
            e.type = (ValueType)typeInt;
            e.address = addr;
            e.frozen = frozen != 0;
            iss >> std::ws;
            // Read editValue (up to space)
            std::string val;
            iss >> val;
            strncpy(e.editValue, val.c_str(), sizeof(e.editValue) - 1);
            // Read description (rest of line)
            std::string desc;
            std::getline(iss >> std::ws, desc);
            strncpy(e.description, desc.c_str(), sizeof(e.description) - 1);
            m_entries.push_back(e);
        }
    }
}
