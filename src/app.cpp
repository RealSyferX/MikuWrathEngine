#include "app.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <sstream>

bool App::Stristr(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != haystack.end();
}

App::App() {
    m_scanner.SetProcess(&m_process);
    m_memViewer.SetProcess(&m_process);
    m_memViewer.SetDisassembler(&m_disasm);
    m_memViewer.SetAddToTableCallback([this](uintptr_t addr, ValueType type) {
        m_table.Add(addr, type);
    });
}

App::~App() {
    m_process.CloseTarget();
}

// ============================================================
// Custom title bar for borderless overlay
// ============================================================
void App::RenderTitleBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.14f, 1.0f));
    ImGui::BeginChild("##TitleBar", ImVec2(0, 30), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    float btnW = 38;
    float btnH = 28;
    float winW = ImGui::GetWindowWidth();

    // App name
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.65f, 1.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("  MikuWrathEngine");
    ImGui::PopStyleColor();

    // Right-side buttons: pin | minimize | maximize | close
    ImGui::SameLine(winW - btnW * 4 + 2);

    // Pin (always-on-top toggle)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));
    if (m_alwaysOnTop)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.70f, 1.0f, 1.0f));
    else
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.50f, 1.0f));
    if (ImGui::Button(" ^", ImVec2(btnW, btnH))) {
        m_alwaysOnTop = !m_alwaysOnTop;
        SetWindowPos(m_hwnd, m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    ImGui::PopStyleColor(4);

    // Minimize
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));
    if (ImGui::Button(" -", ImVec2(btnW, btnH))) {
        ShowWindow(m_hwnd, SW_MINIMIZE);
    }
    ImGui::PopStyleColor(3);

    // Maximize / Restore
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));
    bool maximized = IsZoomed(m_hwnd);
    const char* maxIcon = maximized ? " #" : " []";
    if (ImGui::Button(maxIcon, ImVec2(btnW, btnH))) {
        if (maximized) ShowWindow(m_hwnd, SW_RESTORE);
        else ShowWindow(m_hwnd, SW_MAXIMIZE);
    }
    ImGui::PopStyleColor(3);

    // Close
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.05f, 0.05f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(" x", ImVec2(btnW, btnH))) {
        PostQuitMessage(0);
    }
    ImGui::PopStyleColor(4);

    // Drag detection: click on empty title bar area
    if (m_hwnd && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(0)) {
            m_pendingDrag = true;
        }
        // Double-click toggles maximize
        if (ImGui::IsMouseDoubleClicked(0)) {
            m_pendingDrag = false;
            if (IsZoomed(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
            else ShowWindow(m_hwnd, SW_MAXIMIZE);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================
// Main render
// ============================================================
void App::Render() {
    float dt = ImGui::GetIO().DeltaTime;

    // Update frozen entries
    m_table.UpdateFrozen(m_process, dt);

    // Update address table values periodically
    m_updateTimer += dt;
    if (m_updateTimer >= 0.25f) {
        m_table.UpdateValues(m_process);
        m_updateTimer = 0.0f;
    }

    // Full-viewport borderless main window
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    bool open = true;
    if (ImGui::Begin("##MainOverlay", &open, flags)) {
        ImGui::PopStyleVar(3);

        // Custom title bar
        RenderTitleBar();

        // Menu bar
        RenderMenuBar();

        // Content with padding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGui::BeginChild("##Content", ImVec2(0, 0), false);
        ImGui::PopStyleVar();

        RenderProcessBar();
        ImGui::Separator();
        RenderScanPanel();
        ImGui::Separator();

        float resultsH = ImGui::GetContentRegionAvail().y * 0.50f;
        ImGui::BeginChild("ResultsPanel", ImVec2(0, resultsH), ImGuiChildFlags_Borders);
        RenderResults();
        ImGui::EndChild();

        ImGui::BeginChild("TablePanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
        m_table.Render(m_process);
        ImGui::EndChild();

        ImGui::EndChild();
    } else {
        ImGui::PopStyleVar(3);
    }
    ImGui::End();

    if (m_showMemViewer) {
        m_memViewer.Render();
    }

    if (m_showProcessPicker) {
        RenderProcessPicker();
    }

    if (m_showRegionList) {
        RenderRegionList();
    }

    if (m_showModuleList) {
        RenderModuleList();
    }

    HandleHotkeys();

    if (!open) {
        PostQuitMessage(0);
    }
}

// ============================================================
// Menu bar
// ============================================================
void App::RenderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Table")) { m_table.Save("miku_table.mwt"); }
            if (ImGui::MenuItem("Load Table")) { m_table.Load("miku_table.mwt"); }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { PostQuitMessage(0); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Memory Viewer", "Ctrl+M")) {
                m_showMemViewer = !m_showMemViewer;
            }
            if (ImGui::MenuItem("Memory Regions")) {
                m_showRegionList = !m_showRegionList;
            }
            if (ImGui::MenuItem("Module List")) {
                m_showModuleList = !m_showModuleList;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("MikuWrathEngine v1.1");
            ImGui::TextDisabled("Cheat Engine Clone - Overlay Edition");
            ImGui::Separator();
            ImGui::TextDisabled("Hotkeys:");
            ImGui::TextDisabled("  Ctrl+F - Scan | Ctrl+R - Reset");
            ImGui::TextDisabled("  Ctrl+O - Open Process | Ctrl+S - Save");
            ImGui::TextDisabled("  Ctrl+M - Memory Viewer | Ctrl+G - Go Addr");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Hotkeys
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M)) {
        m_showMemViewer = !m_showMemViewer;
    }
}

// ============================================================
// Hotkeys
// ============================================================
void App::HandleHotkeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl) return;

    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        if (m_process.IsOpen() && !m_scanner.IsScanning()) {
            if (m_scanner.IsFirstScan()) DoNewScan();
            else DoNextScan();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        DoResetScan();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_O)) {
        m_processList = m_process.EnumerateProcesses();
        m_showProcessPicker = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S)) {
        m_table.Save("miku_table.mwt");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G)) {
        m_showMemViewer = true;
    }
}

// ============================================================
// Memory region list
// ============================================================
void App::RenderRegionList() {
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory Regions", &m_showRegionList, ImGuiWindowFlags_NoCollapse)) {
        if (!m_process.IsOpen()) {
            ImGui::TextDisabled("No process selected");
            ImGui::End();
            return;
        }

        if (ImGui::Button("Refresh")) {
            // Regions are enumerated on demand
        }

        auto regions = m_process.EnumerateRegions(false);

        if (ImGui::BeginTable("RegionTable", 5,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
            ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Protection", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Writable", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)regions.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    auto& r = regions[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    ImGui::TableNextColumn();
                    char baseStr[32];
                    snprintf(baseStr, sizeof(baseStr), "0x%016llX", (unsigned long long)r.base);
                    if (ImGui::Selectable(baseStr, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            m_memViewer.GoToAddress(r.base);
                            m_showMemViewer = true;
                        }
                    }

                    ImGui::TableNextColumn();
                    char sizeStr[32];
                    snprintf(sizeStr, sizeof(sizeStr), "0x%llX (%zu KB)",
                        (unsigned long long)r.size, r.size / 1024);
                    ImGui::TextUnformatted(sizeStr);

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%016llX", (unsigned long long)(r.base + r.size));

                    ImGui::TableNextColumn();
                    const char* protStr = "?";
                    switch (r.protect) {
                    case PAGE_NOACCESS:             protStr = "NOACCESS"; break;
                    case PAGE_READONLY:             protStr = "READONLY"; break;
                    case PAGE_READWRITE:            protStr = "READWRITE"; break;
                    case PAGE_WRITECOPY:            protStr = "WRITECOPY"; break;
                    case PAGE_EXECUTE:              protStr = "EXECUTE"; break;
                    case PAGE_EXECUTE_READ:         protStr = "EXECUTE_READ"; break;
                    case PAGE_EXECUTE_READWRITE:    protStr = "EXECUTE_RW"; break;
                    case PAGE_EXECUTE_WRITECOPY:    protStr = "EXECUTE_WC"; break;
                    default:                        protStr = "OTHER"; break;
                    }
                    ImGui::TextUnformatted(protStr);

                    ImGui::TableNextColumn();
                    ImGui::Text(r.writable ? "Yes" : "No");

                    ImGui::PopID();
                }
            }
            clipper.End();
            ImGui::EndTable();
        }

        ImGui::Text("Total regions: %zu", regions.size());
    }
    ImGui::End();
}

// ============================================================
// Module list
// ============================================================
void App::RenderModuleList() {
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Module List", &m_showModuleList, ImGuiWindowFlags_NoCollapse)) {
        if (!m_process.IsOpen()) {
            ImGui::TextDisabled("No process selected");
            ImGui::End();
            return;
        }

        auto mods = m_process.EnumerateModules();

        if (ImGui::BeginTable("ModuleTable", 4,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)mods.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    auto& m = mods[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(m.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            m_memViewer.GoToAddress(m.base);
                            m_showMemViewer = true;
                        }
                    }
                    if (ImGui::BeginPopupContextItem("ModCtx")) {
                        if (ImGui::MenuItem("Browse in Memory Viewer")) {
                            m_memViewer.GoToAddress(m.base);
                            m_showMemViewer = true;
                        }
                        if (ImGui::MenuItem("Copy Base Address")) {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)m.base);
                            ImGui::SetClipboardText(buf);
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%016llX", (unsigned long long)m.base);

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%llX", (unsigned long long)m.size);

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%016llX", (unsigned long long)(m.base + m.size));

                    ImGui::PopID();
                }
            }
            clipper.End();
            ImGui::EndTable();
        }

        ImGui::Text("Total modules: %zu", mods.size());
    }
    ImGui::End();
}

// ============================================================
// Process bar
// ============================================================
void App::RenderProcessBar() {
    if (m_process.IsOpen()) {
        ImVec4 green(0.2f, 0.8f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, green);
        ImGui::Text("[*] %s (PID: %lu) [%s]",
            m_process.GetName().c_str(),
            m_process.GetPid(),
            m_process.Is64Bit() ? "x64" : "x86");
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
        std::string path = m_process.GetProcessPath();
        if (!path.empty()) {
            ImGui::TextDisabled("  Path: %s", path.c_str());
        }
        ImGui::PopStyleColor();
    } else {
        ImVec4 red(0.9f, 0.3f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, red);
        ImGui::Text("[ ] No process selected");
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    if (ImGui::Button("Open Process")) {
        m_processList = m_process.EnumerateProcesses();
        m_showProcessPicker = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Memory Viewer")) {
        m_showMemViewer = !m_showMemViewer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Regions")) {
        m_showRegionList = !m_showRegionList;
    }
    ImGui::SameLine();
    if (ImGui::Button("Modules")) {
        m_showModuleList = !m_showModuleList;
    }
    ImGui::SameLine();
    if (m_process.IsOpen()) {
        if (ImGui::Button("Close Process")) {
            m_process.CloseTarget();
            m_scanner.Reset();
        }
    }
}

// ============================================================
// Scan panel
// ============================================================
void App::RenderScanPanel() {
    bool canScan = m_process.IsOpen() && !m_scanner.IsScanning();
    bool isFirst = m_scanner.IsFirstScan();

    // Value type combo
    {
        const char* typeNames[] = {"Byte", "2 Bytes", "4 Bytes", "8 Bytes",
                                   "Float", "Double", "String", "AOB"};
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Type:");
        ImGui::SameLine();
        ImGui::PushItemWidth(100);
        ImGui::Combo("##TypeCombo", &m_typeIdx, typeNames, 8);
        ImGui::PopItemWidth();
    }

    ImGui::SameLine();

    // Scan type combo
    {
        if (isFirst) {
            const char* scanTypes[] = {"Exact Value", "Bigger Than", "Smaller Than",
                                       "Between", "Unknown Init Value"};
            ImGui::Text("Scan:");
            ImGui::SameLine();
            ImGui::PushItemWidth(130);
            ImGui::Combo("##ScanCombo", &m_scanTypeIdx, scanTypes, 5);
            ImGui::PopItemWidth();
        } else {
            const char* nextTypes[] = {"Exact Value", "Bigger Than", "Smaller Than",
                                       "Between", "Changed", "Unchanged",
                                       "Increased", "Decreased"};
            ImGui::Text("Next:");
            ImGui::SameLine();
            ImGui::PushItemWidth(130);
            ImGui::Combo("##NextCombo", &m_nextScanTypeIdx, nextTypes, 8);
            ImGui::PopItemWidth();
        }
    }

    ImGui::SameLine();

    // Hex checkbox (only for integer types)
    if (CurrentType() <= ValueType::Qword) {
        ImGui::Checkbox("Hex", &m_hexMode);
        ImGui::SameLine();
    }

    ImGui::Checkbox("Writable Only", &m_writableOnly);

    // Value input(s)
    bool unknownInit = (isFirst && m_scanTypeIdx == 4);
    bool isBetween = (isFirst ? m_scanTypeIdx == 3 : m_nextScanTypeIdx == 3);
    bool needValue = !unknownInit;

    // For changed/unchanged/increased/decreased, no value input needed
    if (!isFirst && m_nextScanTypeIdx >= 4) {
        needValue = false;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Value:");
    ImGui::SameLine();

    if (!needValue) {
        ImGui::BeginDisabled();
    }

    ImGui::PushItemWidth(200);
    if (unknownInit) {
        ImGui::InputText("##ValDisabled", m_valueBuf, sizeof(m_valueBuf),
                         ImGuiInputTextFlags_ReadOnly);
    } else {
        if (CurrentType() == ValueType::AOB) {
            ImGui::InputTextWithHint("##Val1", "e.g. 7F ?? 90 41", m_valueBuf, sizeof(m_valueBuf));
        } else if (CurrentType() == ValueType::String) {
            ImGui::InputText("##Val1", m_valueBuf, sizeof(m_valueBuf));
        } else {
            ImGui::InputText("##Val1", m_valueBuf, sizeof(m_valueBuf));
        }
    }
    ImGui::PopItemWidth();

    if (isBetween) {
        ImGui::SameLine();
        ImGui::Text("to");
        ImGui::SameLine();
        ImGui::PushItemWidth(200);
        ImGui::InputText("##Val2", m_valueBuf2, sizeof(m_valueBuf2));
        ImGui::PopItemWidth();
    }

    if (!needValue) {
        ImGui::EndDisabled();
    }

    // Scan buttons
    ImGui::SameLine();

    if (!canScan) {
        ImGui::BeginDisabled();
    }

    if (isFirst) {
        if (ImGui::Button("First Scan", ImVec2(100, 0))) {
            DoNewScan();
        }
    } else {
        if (ImGui::Button("Next Scan", ImVec2(100, 0))) {
            DoNextScan();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(60, 0))) {
            DoResetScan();
        }
    }

    if (!canScan) {
        ImGui::EndDisabled();
    }

    // Progress bar
    if (m_scanner.IsScanning()) {
        ImGui::SameLine();
        char progText[64];
        snprintf(progText, sizeof(progText), "Scanning... %.0f%%", m_scanner.GetProgress() * 100.0f);
        ImGui::ProgressBar(m_scanner.GetProgress(), ImVec2(150, 0), progText);
    }

    // Result count
    ImGui::AlignTextToFramePadding();
    size_t count = m_scanner.GetResultCount();
    if (count > 100000) {
        ImGui::Text("Found: %zu (showing first 100000)", count);
    } else {
        ImGui::Text("Found: %zu", count);
    }
}

// ============================================================
// Scan actions
// ============================================================
void App::DoNewScan() {
    ValueType type = CurrentType();
    m_scanner.NewScanAsync(type, m_scanTypeIdx,
                           m_valueBuf, m_valueBuf2,
                           m_hexMode, m_writableOnly);
    m_selectedResult = -1;
}

void App::DoNextScan() {
    m_scanner.NextScanAsync(m_nextScanTypeIdx,
                            m_valueBuf, m_valueBuf2,
                            m_hexMode);
    m_selectedResult = -1;
}

void App::DoResetScan() {
    m_scanner.Reset();
    m_selectedResult = -1;
}

// ============================================================
// Results list
// ============================================================
void App::RenderResults() {
    if (m_scanner.IsScanning()) {
        ImGui::TextDisabled("Scanning in progress...");
        return;
    }

    auto results = m_scanner.GetResultsCopy();
    size_t displayCount = std::min(results.size(), (size_t)100000);

    if (displayCount == 0) {
        ImGui::TextDisabled("No results. Run a scan to find addresses.");
        return;
    }

    if (ImGui::BeginTable("ResultsTable", 2,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {

        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)displayCount);

        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                ImGui::TableNextRow();
                ImGui::PushID(i);

                // Address
                ImGui::TableNextColumn();
                char addrLabel[32];
                snprintf(addrLabel, sizeof(addrLabel), "0x%016llX",
                    (unsigned long long)results[i]);
                bool selected = (m_selectedResult == i);
                if (ImGui::Selectable(addrLabel, &selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_selectedResult = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        m_table.Add(results[i], m_scanner.GetValueType());
                    }
                }

                // Context menu
                if (ImGui::BeginPopupContextItem("ResultCtx")) {
                    if (ImGui::MenuItem("Add to Address Table")) {
                        m_table.Add(results[i], m_scanner.GetValueType());
                    }
                    if (ImGui::MenuItem("Browse Memory Region")) {
                        m_memViewer.GoToAddress(results[i]);
                        m_showMemViewer = true;
                    }
                    ImGui::EndPopup();
                }

                // Value
                ImGui::TableNextColumn();
                std::string val = m_scanner.ReadValueString(results[i]);
                ImGui::TextUnformatted(val.c_str());

                ImGui::PopID();
            }
        }
        clipper.End();

        ImGui::EndTable();
    }
}

// ============================================================
// Process picker
// ============================================================
void App::RenderProcessPicker() {
    ImGui::SetNextWindowSize(ImVec2(450, 450), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Select Process", &m_showProcessPicker, ImGuiWindowFlags_NoCollapse)) {
        ImGui::InputTextWithHint("##Filter", "Filter process name...", m_processFilter, sizeof(m_processFilter));
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            m_processList = m_process.EnumerateProcesses();
        }

        ImGui::Separator();

        // Build filtered list
        std::vector<ProcessInfo*> filtered;
        std::string filterStr = m_processFilter;
        for (auto& p : m_processList) {
            if (Stristr(p.name, filterStr)) {
                filtered.push_back(&p);
            }
        }

        if (ImGui::BeginTable("ProcTable", 2,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Process Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)filtered.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    ImGui::TableNextColumn();
                    char pidLabel[32];
                    snprintf(pidLabel, sizeof(pidLabel), "%lu", filtered[i]->pid);
                    bool selected = (filtered[i]->pid == m_process.GetPid());
                    if (ImGui::Selectable(pidLabel, &selected,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                        // Single click selects
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        if (m_process.OpenTarget(filtered[i]->pid)) {
                            m_disasm.Init(m_process.Is64Bit());
                            m_scanner.Reset();
                            m_showProcessPicker = false;
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(filtered[i]->name.c_str());

                    ImGui::PopID();
                }
            }
            clipper.End();

            ImGui::EndTable();
        }

        ImGui::Separator();
        if (ImGui::Button("Close")) {
            m_showProcessPicker = false;
        }
    }
    ImGui::End();
}
