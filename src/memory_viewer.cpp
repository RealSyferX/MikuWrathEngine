#include "memory_viewer.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

void MemoryViewer::GoToAddress(uintptr_t addr) {
    m_hexAddr = addr;
    m_disasmAddr = addr;
    snprintf(m_addrBuf, sizeof(m_addrBuf), "%llX", (unsigned long long)addr);
    RefreshDisasm();
}

void MemoryViewer::ParseAndGo() {
    char* end = nullptr;
    unsigned long long val = strtoull(m_addrBuf, &end, 16);
    if (end != m_addrBuf) {
        m_hexAddr = (uintptr_t)val;
        m_disasmAddr = (uintptr_t)val;
        RefreshDisasm();
    }
}

void MemoryViewer::RefreshDisasm() {
    m_disasmView.clear();
    if (!m_pm || !m_pm->IsOpen() || !m_dis || !m_dis->IsInitialized()) return;

    uint8_t buf[512];
    if (m_pm->Read(m_disasmAddr, buf, sizeof(buf))) {
        m_disasmView = m_dis->Disassemble(m_disasmAddr, buf, sizeof(buf), 32);
    }
}

void MemoryViewer::ScrollHex(int lines) {
    uintptr_t delta = (uintptr_t)(lines * m_hexCols);
    if (lines < 0) {
        uintptr_t newAddr = m_hexAddr - delta;
        if (newAddr > m_hexAddr) return; // underflow
        m_hexAddr = newAddr;
    } else {
        m_hexAddr += delta;
    }
}

void MemoryViewer::ScrollDisasm(int lines) {
    if (lines > 0) {
        // Scroll down: move to the second instruction
        if (m_disasmView.size() > 1) {
            m_disasmAddr = m_disasmView[1].address;
            // Keep instructions after the first
            m_disasmView.erase(m_disasmView.begin());
            // If running low, append more
            if (m_disasmView.size() < 8) {
                if (!m_disasmView.empty()) {
                    uintptr_t nextAddr = m_disasmView.back().address + m_disasmView.back().size;
                    uint8_t buf[256];
                    if (m_pm->Read(nextAddr, buf, sizeof(buf))) {
                        auto more = m_dis->Disassemble(nextAddr, buf, sizeof(buf), 24);
                        m_disasmView.insert(m_disasmView.end(), more.begin(), more.end());
                    }
                }
            }
        }
    } else if (lines < 0) {
        // Scroll up: find previous instruction
        uintptr_t prevAddr = m_dis->FindPreviousInstruction(m_disasmAddr, m_pm);
        if (prevAddr != m_disasmAddr) {
            m_disasmAddr = prevAddr;
            RefreshDisasm();
        }
    }
}

void MemoryViewer::Render() {
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Viewer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    RenderAddressBar();
    ImGui::Separator();
    RenderDisasmView();
    ImGui::Separator();
    RenderHexView();

    ImGui::End();
}

void MemoryViewer::RenderAddressBar() {
    if (!m_pm || !m_pm->IsOpen()) {
        ImGui::TextDisabled("No process selected");
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Address: 0x");
    ImGui::SameLine();
    ImGui::PushItemWidth(200);
    if (ImGui::InputText("##AddrBar", m_addrBuf, sizeof(m_addrBuf),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        ParseAndGo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Go")) {
        ParseAndGo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Sync Hex to Disasm")) {
        m_hexAddr = m_disasmAddr;
    }
    ImGui::SameLine();
    if (ImGui::Button("Sync Disasm to Hex")) {
        m_disasmAddr = m_hexAddr;
        RefreshDisasm();
    }
}

void MemoryViewer::RenderHexView() {
    ImGui::BeginChild("HexView", ImVec2(0, 0), ImGuiChildFlags_Borders);

    if (!m_pm || !m_pm->IsOpen()) {
        ImGui::TextDisabled("No process");
        ImGui::EndChild();
        return;
    }

    int totalBytes = m_hexLines * m_hexCols;
    std::vector<uint8_t> buf(totalBytes);
    bool ok = m_pm->Read(m_hexAddr, buf.data(), totalBytes);

    if (!ok) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "?? Failed to read at 0x%llX",
            (unsigned long long)m_hexAddr);
    } else {
        for (int line = 0; line < m_hexLines; line++) {
            uintptr_t lineAddr = m_hexAddr + (uintptr_t)(line * m_hexCols);

            char out[512];
            int pos = snprintf(out, sizeof(out), "%016llX  ",
                (unsigned long long)lineAddr);

            for (int col = 0; col < m_hexCols; col++) {
                pos += snprintf(out + pos, sizeof(out) - pos, "%02X ",
                    buf[line * m_hexCols + col]);
            }

            out[pos++] = ' ';
            out[pos++] = '|';
            for (int col = 0; col < m_hexCols; col++) {
                char c = (char)buf[line * m_hexCols + col];
                out[pos++] = (c >= 32 && c < 127) ? c : '.';
            }
            out[pos++] = '|';
            out[pos] = '\0';

            ImGui::TextUnformatted(out);
        }
    }

    // Mouse wheel scrolling
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0) ScrollHex(-1);
        else if (wheel < 0) ScrollHex(1);
    }

    ImGui::EndChild();
}

void MemoryViewer::RenderDisasmView() {
    float halfHeight = ImGui::GetContentRegionAvail().y * 0.5f;
    ImGui::BeginChild("DisasmView", ImVec2(0, halfHeight), ImGuiChildFlags_Borders);

    if (!m_pm || !m_pm->IsOpen()) {
        ImGui::TextDisabled("No process");
        ImGui::EndChild();
        return;
    }

    if (!m_dis || !m_dis->IsInitialized()) {
        ImGui::TextDisabled("Disassembler not initialized");
        ImGui::EndChild();
        return;
    }

    if (m_disasmView.empty()) {
        RefreshDisasm();
    }

    for (size_t i = 0; i < m_disasmView.size(); i++) {
        auto& inst = m_disasmView[i];

        char line[512];
        int pos = snprintf(line, sizeof(line), "%016llX  ",
            (unsigned long long)inst.address);

        int byteLen = std::min((int)inst.size, 8);
        for (int j = 0; j < byteLen; j++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", inst.bytes[j]);
        }
        for (int j = byteLen; j < 8; j++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "   ");
        }
        if (inst.size > 8) {
            pos += snprintf(line + pos, sizeof(line) - pos, ".. ");
        }

        // Pad to align
        while (pos < 52) line[pos++] = ' ';
        pos += snprintf(line + pos, sizeof(line) - pos, "%s %s",
            inst.mnemonic, inst.opStr);

        bool selected = (inst.address == m_selectedAddr);
        ImGui::PushID((int)i);
        if (ImGui::Selectable(line, &selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selectedAddr = inst.address;
            if (ImGui::IsMouseDoubleClicked(0)) {
                m_hexAddr = inst.address;
            }
        }

        if (ImGui::BeginPopupContextItem("DisasmCtx")) {
            if (ImGui::MenuItem("Add to Address Table")) {
                if (m_addToTable) m_addToTable(inst.address, ValueType::Dword);
            }
            if (ImGui::MenuItem("Follow in Hex View")) {
                m_hexAddr = inst.address;
            }
            if (ImGui::MenuItem("Go to Disasm")) {
                m_disasmAddr = inst.address;
                RefreshDisasm();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Mouse wheel scrolling
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0) ScrollDisasm(-1);
        else if (wheel < 0) ScrollDisasm(1);
    }

    ImGui::EndChild();
}
