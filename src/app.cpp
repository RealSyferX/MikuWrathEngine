#include "app.h"
#include "memory_viewer.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

bool App::Stristr(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != haystack.end();
}

App::App() {
    m_scanner.SetProcess(&m_process);
    m_memViewer = std::make_unique<MemoryViewer>();
    m_memViewer->SetProcess(&m_process);
    m_memViewer->SetDisassembler(&m_disasm);
    m_memViewer->SetAddToTableCallback([this](uintptr_t addr, ValueType type) {
        m_table.Add(addr, type);
    });
    m_lastTick = GetTickCount();
}

App::~App() {
    m_process.CloseTarget();
}

// ============================================================
// Layout
// ============================================================
void App::ComputeLayout(int w, int h) {
    int y = 0;
    m_layout.titleBar = { 0, y, w, y + Theme::TITLE_H }; y += Theme::TITLE_H;
    m_layout.menuBar = { 0, y, w, y + Theme::MENU_H }; y += Theme::MENU_H;
    m_layout.processBar = { 0, y, w, y + 28 }; y += 28;
    m_layout.scanPanel = { 0, y, w, y + 52 }; y += 52;

    int remain = h - y;
    int resultsH = remain / 2;
    m_layout.resultsPanel = { 0, y, w, y + resultsH }; y += resultsH;
    m_layout.tablePanel = { 0, y, w, h };
}

// ============================================================
// State update
// ============================================================
void App::UpdateState() {
    DWORD now = GetTickCount();
    // Clamp delta time to prevent bursts after pause/suspend (e.g. debugger,
    // sleep, minimized window) from producing huge steps that destabilize
    // timers and frozen-value updates.
    m_dt = std::min((now - m_lastTick) / 1000.0f, 0.1f);
    m_lastTick = now;

    m_table.UpdateFrozen(m_process, m_dt);

    m_updateTimer += m_dt;
    if (m_updateTimer >= 0.25f) {
        m_table.UpdateValues(m_process);
        m_updateTimer = 0.0f;
    }

    // Detect scan completion and auto-populate results
    bool isScanning = m_scanner.IsScanning();
    if (m_wasScanning && !isScanning) {
        m_results = m_scanner.GetResultsCopy();
        m_cachedResultValues.clear();
    }
    m_wasScanning = isScanning;
}

// ============================================================
// Main paint
// ============================================================
void App::OnPaint(Gdiplus::Graphics* g) {
    int w = m_ui.width, h = m_ui.height;
    ComputeLayout(w, h);
    UpdateState();

    // Background
    UI::FillRect(g, {0, 0, w, h}, Theme::BG_MAIN());

    // Neon border around entire window
    UI::DrawNeonBorder(g, 0, 0, w - 1, h - 1, Theme::NEON());

    // Render overlays or main content
    if (m_showProcessPicker) {
        RenderProcessPicker();
    } else if (m_showRegionList) {
        RenderRegionList();
    } else if (m_showModuleList) {
        RenderModuleList();
    } else {
        RenderTitleBar();
        RenderMenuBar();
        RenderProcessBar();
        RenderScanPanel();
        UI::DrawSeparator(g, m_layout.scanPanel.bottom, 2, w - 2);
        RenderResults();
        UI::DrawSeparator(g, m_layout.resultsPanel.bottom, 2, w - 2);
        RenderAddressTable();
    }

    // Reset one-shot input flags
    m_ui.mousePressed = false;
    m_ui.mouseReleased = false;
    m_ui.mouseDoubleClicked = false;
    m_ui.mouseRightPressed = false;
    m_ui.hasCharInput = false;
    m_ui.keyPressed = false;
    m_ui.mouseWheel = 0;
}

// ============================================================
// Title bar
// ============================================================
void App::RenderTitleBar() {
    auto& rc = m_layout.titleBar;
    UI::FillRect(m_ui.g, rc, Theme::BG_TITLE());

    UI::DrawText(m_ui.g, 8, 7, "MikuWrathEngine", Theme::CLR_DIM());

    int btnW = 36, btnH = 24;
    int x = rc.right - btnW * 4;

    // Pin
    {
        Gdiplus::Color txt = m_alwaysOnTop ? Theme::ACCENT_LIGHT() : Theme::CLR_DIM();
        RECT btn = { x, 3, x + btnW, 3 + btnH };
        bool hovered = m_ui.PtInRect(btn);
        UI::FillRect(m_ui.g, btn, hovered ? Theme::BG_HOVER() : Theme::BG_TITLE());
        UI::DrawText(m_ui.g, btn.left + 10, btn.top + 4, "^", txt);
        if (hovered && m_ui.mousePressed) {
            m_alwaysOnTop = !m_alwaysOnTop;
            SetWindowPos(m_hwnd, m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
    }
    // Minimize
    {
        RECT btn = { x + btnW, 3, x + btnW * 2, 3 + btnH };
        bool hovered = m_ui.PtInRect(btn);
        UI::FillRect(m_ui.g, btn, hovered ? Theme::BG_HOVER() : Theme::BG_TITLE());
        UI::DrawText(m_ui.g, btn.left + 10, btn.top + 4, "-", Theme::CLR_TEXT());
        if (hovered && m_ui.mousePressed) ShowWindow(m_hwnd, SW_MINIMIZE);
    }
    // Maximize
    {
        RECT btn = { x + btnW * 2, 3, x + btnW * 3, 3 + btnH };
        bool hovered = m_ui.PtInRect(btn);
        UI::FillRect(m_ui.g, btn, hovered ? Theme::BG_HOVER() : Theme::BG_TITLE());
        const char* icon = IsZoomed(m_hwnd) ? "#" : "[]";
        UI::DrawText(m_ui.g, btn.left + 8, btn.top + 4, icon, Theme::CLR_TEXT());
        if (hovered && m_ui.mousePressed) {
            if (IsZoomed(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
            else ShowWindow(m_hwnd, SW_MAXIMIZE);
        }
    }
    // Close
    {
        RECT btn = { x + btnW * 3, 3, x + btnW * 4, 3 + btnH };
        bool hovered = m_ui.PtInRect(btn);
        UI::FillRect(m_ui.g, btn, hovered ? Gdiplus::Color(200, 40, 40) : Theme::BG_TITLE());
        UI::DrawText(m_ui.g, btn.left + 10, btn.top + 4, "x", Theme::CLR_TEXT());
        if (hovered && m_ui.mousePressed) PostQuitMessage(0);
    }

    // Drag
    if (m_ui.PtInRect(rc) && m_ui.mouse.x < x && m_ui.mousePressed) {
        m_pendingDrag = true;
    }
    if (m_ui.PtInRect(rc) && m_ui.mouseDoubleClicked) {
        if (IsZoomed(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
        else ShowWindow(m_hwnd, SW_MAXIMIZE);
    }

    UI::DrawSeparator(m_ui.g, rc.bottom, 0, rc.right);
}

// ============================================================
// Menu bar
// ============================================================
void App::RenderMenuBar() {
    auto& rc = m_layout.menuBar;
    UI::FillRect(m_ui.g, rc, Theme::BG_PANEL());

    const char* menus[] = {"File", "Tools", "Help"};
    int x = 4;
    for (int i = 0; i < 3; i++) {
        int tw, th;
        UI::MeasureText(m_ui.g, menus[i], nullptr, &tw, &th);
        RECT btn = { x, rc.top + 2, x + tw + 12, rc.bottom - 2 };
        bool hovered = m_ui.PtInRect(btn);
        bool open = (m_menuOpen == i);

        if (open || hovered) UI::FillRect(m_ui.g, btn, Theme::BG_HOVER());
        UI::DrawText(m_ui.g, x + 6, rc.top + 4, menus[i], Theme::CLR_TEXT());

        if (hovered && m_ui.mousePressed) {
            m_menuOpen = (m_menuOpen == i) ? -1 : i;
            if (m_menuOpen == i) {
                POINT pt = { btn.left, btn.bottom };
                ClientToScreen(m_hwnd, &pt);
                HMENU menu = CreatePopupMenu();
                if (i == 0) {
                    AppendMenuA(menu, MF_STRING, 101, "Save Table");
                    AppendMenuA(menu, MF_STRING, 102, "Load Table");
                    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuA(menu, MF_STRING, 103, "Exit");
                } else if (i == 1) {
                    AppendMenuA(menu, MF_STRING, 201, "Memory Viewer\tCtrl+M");
                    AppendMenuA(menu, MF_STRING, 202, "Memory Regions");
                    AppendMenuA(menu, MF_STRING, 203, "Module List");
                } else {
                    AppendMenuA(menu, MF_STRING, 301, "About MikuWrathEngine");
                }
                int result = (int)TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                    pt.x, pt.y, 0, m_hwnd, nullptr);
                DestroyMenu(menu);
                m_menuOpen = -1;

                switch (result) {
                case 101: m_table.Save("miku_table.mwt"); break;
                case 102: m_table.Load("miku_table.mwt"); break;
                case 103: PostQuitMessage(0); break;
                case 201: ToggleMemoryViewer(); break;
                case 202: m_showRegionList = true; m_cachedRegions.clear(); m_regionCacheTimer = 2.0f; break;
                case 203: m_showModuleList = true; m_cachedModules.clear(); m_moduleCacheTimer = 2.0f; break;
                }
            }
        }
        x += tw + 16;
    }

    UI::DrawSeparator(m_ui.g, rc.bottom, 0, rc.right);
}

// ============================================================
// Process bar
// ============================================================
void App::RenderProcessBar() {
    auto& rc = m_layout.processBar;
    UI::FillRect(m_ui.g, rc, Theme::BG_MAIN());

    int y = rc.top + 4;

    if (m_process.IsOpen()) {
        char info[256];
        snprintf(info, sizeof(info), "[*] %s (PID: %lu) [%s]",
            m_process.GetName().c_str(), m_process.GetPid(),
            m_process.Is64Bit() ? "x64" : "x86");
        UI::DrawText(m_ui.g, 8, y, info, Theme::CLR_GREEN());

        std::string path = m_process.GetProcessPath();
        if (!path.empty()) {
            UI::DrawText(m_ui.g, 8, y + 13, path.c_str(), Theme::CLR_DIM());
        }
    } else {
        UI::DrawText(m_ui.g, 8, y, "[ ] No process selected", Theme::CLR_RED());
    }

    int btnW = 90, btnH = 22;
    int bx = rc.right - btnW * 4 - 8;

    if (UI::Button(m_ui, 10, {bx, y, bx+btnW, y+btnH}, "Open Process")) {
        m_processList = m_process.EnumerateProcesses();
        m_showProcessPicker = true;
    }
    bx += btnW + 4;
    if (UI::Button(m_ui, 11, {bx, y, bx+btnW, y+btnH}, "Mem Viewer")) {
        ToggleMemoryViewer();
    }
    bx += btnW + 4;
    if (UI::Button(m_ui, 12, {bx, y, bx+btnW, y+btnH}, "Regions")) {
        m_showRegionList = true;
        m_cachedRegions.clear();
        m_regionCacheTimer = 2.0f; // force refresh
    }
    bx += btnW + 4;
    if (m_process.IsOpen()) {
        if (UI::Button(m_ui, 13, {bx, y, bx+btnW, y+btnH}, "Close Proc")) {
            m_process.CloseTarget();
            m_scanner.Reset();
            m_results.clear();
            m_cachedResultValues.clear();
            m_cachedResultScroll = -1;
            m_cachedRegions.clear();
            m_cachedModules.clear();
            m_regionCacheTimer = 2.0f;
            m_moduleCacheTimer = 2.0f;
        }
    }
}

// ============================================================
// Scan panel
// ============================================================
void App::RenderScanPanel() {
    auto& rc = m_layout.scanPanel;
    UI::FillRect(m_ui.g, rc, Theme::BG_MAIN());

    int y = rc.top + 4;
    bool canScan = m_process.IsOpen() && !m_scanner.IsScanning();
    bool isFirst = m_scanner.IsFirstScan();

    // Type combo
    const char* typeNames[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","String","AOB"};
    UI::DrawText(m_ui.g, 8, y+3, "Type:", Theme::CLR_TEXT());
    UI::ComboBox(m_ui, 20, {48, y, 148, y+22}, typeNames, 8, &m_typeIdx);

    // Scan type combo
    if (isFirst) {
        const char* scanTypes[] = {"Exact Value","Bigger Than","Smaller Than","Between","Unknown Init"};
        UI::DrawText(m_ui.g, 158, y+3, "Scan:", Theme::CLR_TEXT());
        UI::ComboBox(m_ui, 200, {198, y, 318, y+22}, scanTypes, 5, &m_scanTypeIdx);
    } else {
        const char* nextTypes[] = {"Exact Value","Bigger Than","Smaller Than","Between",
                                    "Changed","Unchanged","Increased","Decreased"};
        UI::DrawText(m_ui.g, 158, y+3, "Next:", Theme::CLR_TEXT());
        UI::ComboBox(m_ui, 201, {198, y, 318, y+22}, nextTypes, 8, &m_nextScanTypeIdx);
    }

    // Hex + Writable checkboxes
    int cx = 328;
    if (CurrentType() <= ValueType::Qword) {
        UI::Checkbox(m_ui, 21, {cx, y, cx+50, y+22}, "Hex", &m_hexMode);
        cx += 55;
    }
    UI::Checkbox(m_ui, 22, {cx, y, cx+100, y+22}, "Writable", &m_writableOnly);

    // Value input(s)
    bool unknownInit = (isFirst && m_scanTypeIdx == 4);
    bool isBetween = (isFirst ? m_scanTypeIdx == 3 : m_nextScanTypeIdx == 3);
    bool needValue = !unknownInit && !(isFirst ? false : m_nextScanTypeIdx >= 4);

    y += 26;
    UI::DrawText(m_ui.g, 8, y+3, "Value:", Theme::CLR_TEXT());

    if (!needValue) {
        // Draw disabled input
        RECT inpRc = {48, y, 248, y+22};
        UI::FillRect(m_ui.g, inpRc, Theme::BG_CONTROL());
        UI::DrawRect(m_ui.g, inpRc, Theme::BORDER());
    } else {
        const char* hint = (CurrentType() == ValueType::AOB) ? "e.g. 7F ?? 90 41" : nullptr;
        if (hint) {
            // Draw hint text when empty
            if (m_valueBuf[0] == '\0') {
                UI::DrawText(m_ui.g, 52, y+3, hint, Theme::CLR_DIM());
            }
        }
        UI::TextInput(m_ui, 23, {48, y, 248, y+22}, m_valueBuf, sizeof(m_valueBuf));
    }

    if (isBetween && needValue) {
        UI::DrawText(m_ui.g, 258, y+3, "to", Theme::CLR_TEXT());
        UI::TextInput(m_ui, 24, {278, y, 478, y+22}, m_valueBuf2, sizeof(m_valueBuf2));
    }

    // Scan buttons
    int bx = 488;
    if (m_scanner.IsScanning()) {
        // Stop button (red)
        RECT btnRc = {bx, y, bx+100, y+22};
        UI::FillRect(m_ui.g, btnRc, Gdiplus::Color(180, 30, 30));
        UI::DrawRect(m_ui.g, btnRc, Gdiplus::Color(220, 50, 50));
        UI::DrawText(m_ui.g, bx+30, y+3, "Stop", Theme::CLR_TEXT());
        if (m_ui.PtInRect(btnRc) && m_ui.mousePressed) {
            m_scanner.RequestCancel();
        }
        // Progress bar
        RECT pbRc = {bx+110, y, bx+260, y+22};
        UI::ProgressBar(m_ui, pbRc, m_scanner.GetProgress());
    } else if (!canScan) {
        // Draw disabled button
        RECT btnRc = {bx, y, bx+100, y+22};
        UI::FillRect(m_ui.g, btnRc, Theme::BG_CONTROL());
        UI::DrawRect(m_ui.g, btnRc, Theme::BORDER());
        UI::DrawText(m_ui.g, bx+20, y+3, isFirst ? "First Scan" : "Next Scan", Theme::CLR_DIM());
    } else {
        if (isFirst) {
            if (UI::Button(m_ui, 25, {bx, y, bx+100, y+22}, "First Scan")) DoNewScan();
        } else {
            if (UI::Button(m_ui, 26, {bx, y, bx+100, y+22}, "Next Scan")) DoNextScan();
            if (UI::Button(m_ui, 27, {bx+108, y, bx+168, y+22}, "Reset")) DoResetScan();
        }
    }

    // Result count — GetResultCount() returns the live atomic count while
    // scanning and m_results.size() otherwise, so it is always accurate.
    size_t count = m_scanner.GetResultCount();
    char countStr[64];
    if (m_scanner.IsScanning()) {
        if (count > 100000)
            snprintf(countStr, sizeof(countStr), "Scanning... Found: %zu (first 100000)", count);
        else
            snprintf(countStr, sizeof(countStr), "Scanning... Found: %zu", count);
    } else if (count > 100000) {
        snprintf(countStr, sizeof(countStr), "Found: %zu (first 100000)", count);
    } else {
        snprintf(countStr, sizeof(countStr), "Found: %zu", count);
    }
    UI::DrawText(m_ui.g, bx+340, y+3, countStr, Theme::CLR_YELLOW());
}

// ============================================================
// Results list
// ============================================================
void App::RenderResults() {
    auto& rc = m_layout.resultsPanel;
    UI::FillRect(m_ui.g, rc, Theme::BG_PANEL());

    if (m_scanner.IsScanning()) {
        UI::DrawText(m_ui.g, 8, rc.top + 4, "Scanning in progress...", Theme::CLR_DIM());
        return;
    }

    // Refresh results if needed
    if (m_results.empty() && m_scanner.GetResultCount() > 0 && m_scanner.IsFirstScan() == false) {
        m_results = m_scanner.GetResultsCopy();
    }
    if (!m_scanner.IsFirstScan() && m_results.empty()) {
        m_results = m_scanner.GetResultsCopy();
    }

    size_t displayCount = std::min(m_results.size(), (size_t)100000);
    if (displayCount == 0) {
        UI::DrawText(m_ui.g, 8, rc.top + 4, "No results. Run a scan to find addresses.", Theme::CLR_DIM());
        return;
    }

    // Column headers
    int colAddr = 4, colVal = 180;
    UI::FillRect(m_ui.g, {rc.left, rc.top, rc.right, rc.top + 18}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, rc.left + colAddr, rc.top + 2, "Address", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colVal, rc.top + 2, "Value", Theme::CLR_DIM());
    UI::DrawSeparator(m_ui.g, rc.top + 18, rc.left, rc.right);

    // Rows
    int rowH = 16;
    int yStart = rc.top + 20;
    int visibleRows = (rc.bottom - yStart) / rowH;

    // Adjust scroll
    int maxScroll = std::max(0, (int)displayCount - visibleRows);
    m_resultsScroll = std::min(m_resultsScroll, maxScroll);
    m_resultsScroll = std::max(0, m_resultsScroll);

    // Mouse wheel
    if (m_ui.PtInRect(rc) && m_ui.mouseWheel != 0) {
        m_resultsScroll -= m_ui.mouseWheel / 120 * 3;
        m_resultsScroll = std::max(0, std::min(m_resultsScroll, maxScroll));
    }

    // Cache result value strings (batched reads instead of per-row RPM).
    // Refresh at most 4x/sec, or immediately when the scroll position changes
    // so values stay mapped to the correct rows.
    bool scrollChanged = (m_cachedResultScroll != m_resultsScroll);
    m_cachedResultScroll = m_resultsScroll;
    m_resultValueTimer += m_dt;
    int visibleEnd = std::min(m_resultsScroll + visibleRows, (int)displayCount);
    int expectedCount = std::max(0, visibleEnd - m_resultsScroll);
    if (m_cachedResultValues.empty() || scrollChanged ||
        (int)m_cachedResultValues.size() != expectedCount ||
        m_resultValueTimer >= 0.25f) {
        m_cachedResultValues.clear();
        m_cachedResultValues.reserve(expectedCount);
        for (int i = m_resultsScroll; i < visibleEnd; i++) {
            m_cachedResultValues.push_back(m_scanner.ReadValueString(m_results[i]));
        }
        m_resultValueTimer = 0.0f;
    }

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_resultsScroll + i;
        if (idx >= (int)displayCount) break;

        int y = yStart + i * rowH;
        bool selected = (m_selectedResult == idx);
        Gdiplus::Color rowBg = selected ? Theme::BG_SELECTED() :
            (i % 2 == 0 ? Theme::BG_PANEL() : Theme::BG_MAIN());
        UI::FillRect(m_ui.g, {rc.left, y, rc.right, y + rowH}, rowBg);

        char addrStr[32];
        snprintf(addrStr, sizeof(addrStr), "0x%016llX", (unsigned long long)m_results[idx]);
        UI::DrawText(m_ui.g, rc.left + colAddr, y + 1, addrStr, Theme::CLR_BLUE());

        // Use cached value (bounds-checked in case visibleRows grew this frame)
        std::string val;
        int cacheIdx = idx - m_resultsScroll;
        if (cacheIdx >= 0 && cacheIdx < (int)m_cachedResultValues.size()) {
            val = m_cachedResultValues[cacheIdx];
        } else {
            val = m_scanner.ReadValueString(m_results[idx]);
        }
        UI::DrawText(m_ui.g, rc.left + colVal, y + 1, val.c_str(), Theme::CLR_TEXT());

        RECT rowRc = {rc.left, y, rc.right, y + rowH};
        if (m_ui.PtInRect(rowRc) && m_ui.mousePressed) {
            m_selectedResult = idx;
        }
        if (m_ui.PtInRect(rowRc) && m_ui.mouseDoubleClicked) {
            m_table.Add(m_results[idx], m_scanner.GetValueType());
        }
        if (m_ui.PtInRect(rowRc) && m_ui.mouseRightPressed) {
            m_selectedResult = idx;
            ShowResultContextMenu(m_ui.mouse.x, m_ui.mouse.y, m_results[idx]);
        }
    }

    // Scrollbar
    if (displayCount > (size_t)visibleRows) {
        int sbW = 12;
        int sbX = rc.right - sbW;
        int sbH = rc.bottom - yStart;
        int thumbH = std::max(20, sbH * visibleRows / (int)displayCount);
        UI::Scrollbar(m_ui, 9000, {sbX, yStart, rc.right, rc.bottom}, thumbH, (int)displayCount, visibleRows, &m_resultsScroll);
    }
}

// ============================================================
// Address table
// ============================================================
void App::RenderAddressTable() {
    auto& rc = m_layout.tablePanel;
    UI::FillRect(m_ui.g, rc, Theme::BG_PANEL());

    // Toolbar
    int ty = rc.top + 3;
    if (UI::Button(m_ui, 30, {4, ty, 70, ty+20}, "Add")) {
        if (m_process.IsOpen()) m_table.Add(0, ValueType::Dword);
    }
    if (UI::Button(m_ui, 31, {78, ty, 144, ty+20}, "Clear All")) m_table.Clear();
    if (UI::Button(m_ui, 32, {152, ty, 200, ty+20}, "Save")) m_table.Save("miku_table.mwt");
    if (UI::Button(m_ui, 33, {208, ty, 256, ty+20}, "Load")) m_table.Load("miku_table.mwt");

    UI::DrawText(m_ui.g, 270, ty+2, "Right-click row for options", Theme::CLR_DIM());

    UI::DrawSeparator(m_ui.g, ty + 22, rc.left, rc.right);

    // Column headers
    int hy = ty + 24;
    int colFz = 4, colDesc = 34, colAddr = 200, colType = 340, colVal = 410, colX = rc.right - 24;
    UI::FillRect(m_ui.g, {rc.left, hy, rc.right, hy + 18}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, rc.left + colFz, hy + 2, "Fz", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colDesc, hy + 2, "Description", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colAddr, hy + 2, "Address", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colType, hy + 2, "Type", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colVal, hy + 2, "Value", Theme::CLR_DIM());

    // Rows
    int rowH = 20;
    int yStart = hy + 20;
    auto& entries = m_table.Entries();
    int visibleRows = (rc.bottom - yStart) / rowH;
    int maxScroll = std::max(0, (int)entries.size() - visibleRows);
    m_table.m_scrollPos = std::min(m_table.m_scrollPos, maxScroll);
    m_table.m_scrollPos = std::max(0, m_table.m_scrollPos);

    if (m_ui.PtInRect(rc) && m_ui.mouseWheel != 0) {
        m_table.m_scrollPos -= m_ui.mouseWheel / 120 * 3;
        m_table.m_scrollPos = std::max(0, std::min(m_table.m_scrollPos, maxScroll));
    }

    const char* typeNames[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","String","AOB"};

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_table.m_scrollPos + i;
        if (idx >= (int)entries.size()) break;
        int y = yStart + i * rowH;
        auto& e = entries[idx];
        bool sel = (m_table.GetSelected() == idx);

        Gdiplus::Color rowBg = sel ? Theme::BG_SELECTED() : (i % 2 ? Theme::BG_MAIN() : Theme::BG_PANEL());
        UI::FillRect(m_ui.g, {rc.left, y, rc.right, y + rowH}, rowBg);

        // Freeze checkbox
        UI::Checkbox(m_ui, 1000 + idx * 6, {rc.left + colFz, y, rc.left + colFz + 20, y + rowH}, "", &e.frozen);

        // Description
        UI::TextInput(m_ui, 1001 + idx * 6, {rc.left + colDesc, y, rc.left + colAddr - 2, y + rowH},
                      e.description, sizeof(e.description));

        // Address (display only)
        char addrStr[32];
        snprintf(addrStr, sizeof(addrStr), "0x%016llX", (unsigned long long)e.address);
        UI::DrawText(m_ui.g, rc.left + colAddr, y + 3, addrStr, Theme::CLR_BLUE());

        // Type combo
        int ti = (int)e.type;
        if (UI::ComboBox(m_ui, 1002 + idx * 6, {rc.left + colType, y, rc.left + colVal - 2, y + rowH},
                         typeNames, 8, &ti)) {
            e.type = (ValueType)ti;
        }

        // Value
        if (!e.isEditing && m_process.IsOpen()) {
            std::string val = ReadValueString(m_process, e.address, e.type);
            strncpy(e.editValue, val.c_str(), sizeof(e.editValue) - 1);
            e.editValue[sizeof(e.editValue) - 1] = '\0';
        }

        bool valChanged = UI::TextInput(m_ui, 1003 + idx * 6,
            {rc.left + colVal, y, colX - 2, y + rowH}, e.editValue, sizeof(e.editValue));
        e.isEditing = (m_ui.focusId == 1003 + idx * 6);
        if (valChanged) {
            if (m_ui.keyPressed && m_ui.keyCode == VK_RETURN) {
                WriteValueStr(e);
                e.isEditing = false;
                m_ui.focusId = -1;
            }
        }

        // Remove button
        if (UI::Button(m_ui, 1004 + idx * 6, {colX, y, colX + 20, y + rowH}, "X")) {
            m_table.Remove(idx);
            break; // entries changed, re-render next frame
        }

        // Right-click context
        RECT rowRc = {rc.left, y, rc.right, y + rowH};
        if (m_ui.PtInRect(rowRc) && m_ui.mouseRightPressed) {
            m_table.SetSelected(idx);
            ShowTableContextMenu(m_ui.mouse.x, m_ui.mouse.y, idx);
        }
    }

    if (entries.empty()) {
        UI::DrawText(m_ui.g, 8, yStart + 4, "No entries. Right-click scan results or double-click to add.", Theme::CLR_DIM());
    }
}

void App::WriteValueStr(AddressEntry& e) {
    if (!m_process.IsOpen()) return;
    try {
        switch (e.type) {
        case ValueType::Byte:    { uint8_t v = (uint8_t)std::stoul(e.editValue, nullptr, 0); m_process.Write(e.address, &v, 1); } break;
        case ValueType::Word:    { uint16_t v = (uint16_t)std::stoul(e.editValue, nullptr, 0); m_process.Write(e.address, &v, 2); } break;
        case ValueType::Dword:   { uint32_t v = (uint32_t)std::stoul(e.editValue, nullptr, 0); m_process.Write(e.address, &v, 4); } break;
        case ValueType::Qword:   { uint64_t v = (uint64_t)std::stoull(e.editValue, nullptr, 0); m_process.Write(e.address, &v, 8); } break;
        case ValueType::Float32: { float v = std::stof(e.editValue); m_process.Write(e.address, &v, 4); } break;
        case ValueType::Float64: { double v = std::stod(e.editValue); m_process.Write(e.address, &v, 8); } break;
        case ValueType::String:  { m_process.Write(e.address, e.editValue, strlen(e.editValue)+1); } break;
        default: break;
        }
    } catch (...) {}
}

// ============================================================
// Process picker overlay
// ============================================================
void App::RenderProcessPicker() {
    int w = m_ui.width, h = m_ui.height;
    UI::FillRect(m_ui.g, {0, 0, w, h}, Theme::BG_MAIN());
    UI::DrawNeonBorder(m_ui.g, 0, 0, w-1, h-1, Theme::NEON());

    // Title
    UI::FillRect(m_ui.g, {0, 0, w, 30}, Theme::BG_TITLE());
    UI::DrawText(m_ui.g, 8, 7, "Select Process", Theme::CLR_TEXT());

    // Filter
    UI::DrawText(m_ui.g, 8, 35, "Filter:", Theme::CLR_TEXT());
    UI::TextInput(m_ui, 50, {50, 32, w - 160, 54}, m_processFilter, sizeof(m_processFilter));

    if (UI::Button(m_ui, 51, {w - 145, 32, w - 85, 54}, "Refresh")) {
        m_processList = m_process.EnumerateProcesses();
    }
    if (UI::Button(m_ui, 52, {w - 80, 32, w - 10, 54}, "Close")) {
        m_showProcessPicker = false;
    }

    UI::DrawSeparator(m_ui.g, 56, 2, w - 2);

    // Build filtered list
    std::vector<ProcessInfo*> filtered;
    std::string filterStr = m_processFilter;
    for (auto& p : m_processList) {
        if (Stristr(p.name, filterStr)) filtered.push_back(&p);
    }

    // List
    int rowH = 18;
    int yStart = 60;
    int visibleRows = (h - yStart - 30) / rowH;
    int maxScroll = std::max(0, (int)filtered.size() - visibleRows);
    m_processScroll = std::min(m_processScroll, maxScroll);
    m_processScroll = std::max(0, m_processScroll);

    if (m_ui.mouseWheel != 0) {
        m_processScroll -= m_ui.mouseWheel / 120 * 3;
        m_processScroll = std::max(0, std::min(m_processScroll, maxScroll));
    }

    // Headers
    UI::FillRect(m_ui.g, {0, yStart - 2, w, yStart + 16}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, 8, yStart, "PID", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 80, yStart, "Process Name", Theme::CLR_DIM());

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_processScroll + i;
        if (idx >= (int)filtered.size()) break;
        int y = yStart + 20 + i * rowH;
        bool sel = (m_selectedProcess == idx);

        Gdiplus::Color rowBg = sel ? Theme::BG_SELECTED() : (i % 2 ? Theme::BG_MAIN() : Theme::BG_PANEL());
        UI::FillRect(m_ui.g, {0, y, w, y + rowH}, rowBg);

        char pidStr[32]; snprintf(pidStr, sizeof(pidStr), "%lu", filtered[idx]->pid);
        UI::DrawText(m_ui.g, 8, y + 1, pidStr, Theme::CLR_BLUE());
        UI::DrawText(m_ui.g, 80, y + 1, filtered[idx]->name.c_str(), Theme::CLR_TEXT());

        RECT rowRc = {0, y, w, y + rowH};
        if (m_ui.PtInRect(rowRc) && m_ui.mousePressed) {
            m_selectedProcess = idx;
        }
        if (m_ui.PtInRect(rowRc) && m_ui.mouseDoubleClicked) {
            if (m_process.OpenTarget(filtered[idx]->pid)) {
                m_disasm.Init(m_process.Is64Bit());
                m_scanner.Reset();
                m_results.clear();
                // Caches belong to the previous process; invalidate everything.
                m_cachedResultValues.clear();
                m_cachedResultScroll = -1;
                m_cachedRegions.clear();
                m_cachedModules.clear();
                m_regionCacheTimer = 2.0f;
                m_moduleCacheTimer = 2.0f;
                m_showProcessPicker = false;
            }
        }
    }

    // Scrollbar
    if (filtered.size() > (size_t)visibleRows) {
        int sbW = 12, sbX = w - sbW;
        int sbH = h - yStart - 20 - 30;
        int thumbH = std::max(20, sbH * visibleRows / (int)filtered.size());
        UI::Scrollbar(m_ui, 9001, {sbX, yStart + 20, w, h - 30}, thumbH, (int)filtered.size(), visibleRows, &m_processScroll);
    }

    UI::DrawText(m_ui.g, 8, h - 22, "Double-click a process to attach", Theme::CLR_DIM());
}

// ============================================================
// Region list overlay
// ============================================================
void App::RenderRegionList() {
    int w = m_ui.width, h = m_ui.height;
    UI::FillRect(m_ui.g, {0, 0, w, h}, Theme::BG_MAIN());
    UI::DrawNeonBorder(m_ui.g, 0, 0, w-1, h-1, Theme::NEON());

    UI::FillRect(m_ui.g, {0, 0, w, 30}, Theme::BG_TITLE());
    UI::DrawText(m_ui.g, 8, 7, "Memory Regions", Theme::CLR_TEXT());
    if (UI::Button(m_ui, 60, {w - 80, 3, w - 10, 27}, "Back")) {
        m_showRegionList = false;
    }

    if (!m_process.IsOpen()) {
        UI::DrawText(m_ui.g, 8, 35, "No process selected", Theme::CLR_DIM());
        return;
    }

    // Cache region enumeration (refresh every 2s, not every frame)
    m_regionCacheTimer += m_dt;
    if (m_cachedRegions.empty() || m_regionCacheTimer >= 2.0f) {
        m_cachedRegions = m_process.EnumerateRegions(false);
        m_regionCacheTimer = 0.0f;
    }
    auto& regions = m_cachedRegions;
    int rowH = 18;
    int yStart = 35;
    int visibleRows = (h - yStart - 10) / rowH;
    int maxScroll = std::max(0, (int)regions.size() - visibleRows);
    m_regionScroll = std::min(m_regionScroll, maxScroll);
    m_regionScroll = std::max(0, m_regionScroll);

    if (m_ui.mouseWheel != 0) {
        m_regionScroll -= m_ui.mouseWheel / 120 * 3;
        m_regionScroll = std::max(0, std::min(m_regionScroll, maxScroll));
    }

    // Headers
    UI::FillRect(m_ui.g, {0, yStart, w, yStart + 18}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, 8, yStart + 2, "Base", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 170, yStart + 2, "Size", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 310, yStart + 2, "Protection", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 420, yStart + 2, "RW", Theme::CLR_DIM());

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_regionScroll + i;
        if (idx >= (int)regions.size()) break;
        int y = yStart + 20 + i * rowH;
        auto& r = regions[idx];
        Gdiplus::Color rowBg = i % 2 ? Theme::BG_MAIN() : Theme::BG_PANEL();
        UI::FillRect(m_ui.g, {0, y, w, y + rowH}, rowBg);

        char buf[32];
        snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)r.base);
        UI::DrawText(m_ui.g, 8, y + 1, buf, Theme::CLR_BLUE());

        snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)r.size);
        UI::DrawText(m_ui.g, 170, y + 1, buf, Theme::CLR_TEXT());

        const char* ps = "?";
        switch (r.protect) {
        case PAGE_READONLY: ps = "READONLY"; break;
        case PAGE_READWRITE: ps = "READWRITE"; break;
        case PAGE_WRITECOPY: ps = "WRITECOPY"; break;
        case PAGE_EXECUTE: ps = "EXECUTE"; break;
        case PAGE_EXECUTE_READ: ps = "EXEC_R"; break;
        case PAGE_EXECUTE_READWRITE: ps = "EXEC_RW"; break;
        default: ps = "OTHER"; break;
        }
        UI::DrawText(m_ui.g, 310, y + 1, ps, Theme::CLR_TEXT());
        UI::DrawText(m_ui.g, 420, y + 1, r.writable ? "Yes" : "No", r.writable ? Theme::CLR_GREEN() : Theme::CLR_RED());

        RECT rowRc = {0, y, w, y + rowH};
        if (m_ui.PtInRect(rowRc) && m_ui.mouseDoubleClicked) {
            GoToAddress(r.base);
            m_showRegionList = false;
        }
    }

    // Scrollbar
    if (regions.size() > (size_t)visibleRows) {
        int sbW = 12, sbX = w - sbW;
        int sbH = (h - 20) - (yStart + 20);
        int thumbH = std::max(20, sbH * visibleRows / (int)regions.size());
        UI::Scrollbar(m_ui, 9002, {sbX, yStart + 20, w, h - 20}, thumbH, (int)regions.size(), visibleRows, &m_regionScroll);
    }

    char info[64]; snprintf(info, sizeof(info), "Total: %zu regions", regions.size());
    UI::DrawText(m_ui.g, 8, h - 18, info, Theme::CLR_DIM());
}

// ============================================================
// Module list overlay
// ============================================================
void App::RenderModuleList() {
    int w = m_ui.width, h = m_ui.height;
    UI::FillRect(m_ui.g, {0, 0, w, h}, Theme::BG_MAIN());
    UI::DrawNeonBorder(m_ui.g, 0, 0, w-1, h-1, Theme::NEON());

    UI::FillRect(m_ui.g, {0, 0, w, 30}, Theme::BG_TITLE());
    UI::DrawText(m_ui.g, 8, 7, "Module List", Theme::CLR_TEXT());
    if (UI::Button(m_ui, 70, {w - 80, 3, w - 10, 27}, "Back")) {
        m_showModuleList = false;
    }

    if (!m_process.IsOpen()) {
        UI::DrawText(m_ui.g, 8, 35, "No process selected", Theme::CLR_DIM());
        return;
    }

    // Cache module enumeration (refresh every 2s, not every frame)
    m_moduleCacheTimer += m_dt;
    if (m_cachedModules.empty() || m_moduleCacheTimer >= 2.0f) {
        m_cachedModules = m_process.EnumerateModules();
        m_moduleCacheTimer = 0.0f;
    }
    auto& mods = m_cachedModules;
    int rowH = 18;
    int yStart = 35;
    int visibleRows = (h - yStart - 10) / rowH;
    int maxScroll = std::max(0, (int)mods.size() - visibleRows);
    m_moduleScroll = std::min(m_moduleScroll, maxScroll);
    m_moduleScroll = std::max(0, m_moduleScroll);

    if (m_ui.mouseWheel != 0) {
        m_moduleScroll -= m_ui.mouseWheel / 120 * 3;
        m_moduleScroll = std::max(0, std::min(m_moduleScroll, maxScroll));
    }

    UI::FillRect(m_ui.g, {0, yStart, w, yStart + 18}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, 8, yStart + 2, "Module", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 250, yStart + 2, "Base", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 410, yStart + 2, "Size", Theme::CLR_DIM());

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_moduleScroll + i;
        if (idx >= (int)mods.size()) break;
        int y = yStart + 20 + i * rowH;
        auto& m = mods[idx];
        Gdiplus::Color rowBg = i % 2 ? Theme::BG_MAIN() : Theme::BG_PANEL();
        UI::FillRect(m_ui.g, {0, y, w, y + rowH}, rowBg);

        UI::DrawText(m_ui.g, 8, y + 1, m.name.c_str(), Theme::CLR_TEXT());
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)m.base);
        UI::DrawText(m_ui.g, 250, y + 1, buf, Theme::CLR_BLUE());
        snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)m.size);
        UI::DrawText(m_ui.g, 410, y + 1, buf, Theme::CLR_TEXT());

        RECT rowRc = {0, y, w, y + rowH};
        if (m_ui.PtInRect(rowRc) && m_ui.mouseDoubleClicked) {
            GoToAddress(m.base);
            m_showModuleList = false;
        }
        if (m_ui.PtInRect(rowRc) && m_ui.mouseRightPressed) {
            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING, 1, "Browse in Memory Viewer");
            AppendMenuA(menu, MF_STRING, 2, "Copy Base Address");
            POINT pt = {m_ui.mouse.x, m_ui.mouse.y};
            ClientToScreen(m_hwnd, &pt);
            int res = (int)TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
            DestroyMenu(menu);
            if (res == 1) { GoToAddress(m.base); m_showModuleList = false; }
            if (res == 2) {
                char ab[32]; snprintf(ab, sizeof(ab), "0x%llX", (unsigned long long)m.base);
                CopyToClipboard(m_hwnd, ab);
            }
        }
    }

    // Scrollbar
    if (mods.size() > (size_t)visibleRows) {
        int sbW = 12, sbX = w - sbW;
        int sbH = (h - 20) - (yStart + 20);
        int thumbH = std::max(20, sbH * visibleRows / (int)mods.size());
        UI::Scrollbar(m_ui, 9003, {sbX, yStart + 20, w, h - 20}, thumbH, (int)mods.size(), visibleRows, &m_moduleScroll);
    }

    char info[64]; snprintf(info, sizeof(info), "Total: %zu modules", mods.size());
    UI::DrawText(m_ui.g, 8, h - 18, info, Theme::CLR_DIM());
}

// ============================================================
// Context menus
// ============================================================
void App::ShowResultContextMenu(int x, int y, uintptr_t addr) {
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, 1, "Add to Address Table");
    AppendMenuA(menu, MF_STRING, 2, "Browse Memory Region");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, 3, "Copy Address");
    POINT pt = {x, y}; ClientToScreen(m_hwnd, &pt);
    int res = (int)TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    switch (res) {
    case 1: m_table.Add(addr, m_scanner.GetValueType()); break;
    case 2: GoToAddress(addr); break;
    case 3: {
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
        CopyToClipboard(m_hwnd, buf);
    } break;
    }
}

void App::ShowTableContextMenu(int x, int y, size_t entryIdx) {
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, 1, "Browse in Memory Viewer");
    AppendMenuA(menu, MF_STRING, 2, "Copy Address");
    AppendMenuA(menu, MF_STRING, 3, "Copy Value");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, 4, "Delete Entry");
    POINT pt = {x, y}; ClientToScreen(m_hwnd, &pt);
    int res = (int)TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    auto& entries = m_table.Entries();
    if (entryIdx >= entries.size()) return;
    switch (res) {
    case 1: GoToAddress(entries[entryIdx].address); break;
    case 2: {
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)entries[entryIdx].address);
        CopyToClipboard(m_hwnd, buf);
    } break;
    case 3: {
        CopyToClipboard(m_hwnd, entries[entryIdx].editValue);
    } break;
    case 4: m_table.Remove(entryIdx); break;
    }
}

// ============================================================
// Actions
// ============================================================
void App::DoNewScan() {
    m_scanner.NewScanAsync(CurrentType(), m_scanTypeIdx, m_valueBuf, m_valueBuf2, m_hexMode, m_writableOnly);
    m_selectedResult = -1;
    m_results.clear();
    m_cachedResultValues.clear();
    m_cachedResultScroll = -1;
}

void App::DoNextScan() {
    m_scanner.NextScanAsync(m_nextScanTypeIdx, m_valueBuf, m_valueBuf2, m_hexMode);
    m_selectedResult = -1;
    m_results.clear();
    m_cachedResultValues.clear();
    m_cachedResultScroll = -1;
}

void App::DoResetScan() {
    m_scanner.Reset();
    m_selectedResult = -1;
    m_results.clear();
    m_cachedResultValues.clear();
    m_cachedResultScroll = -1;
}

void App::ToggleMemoryViewer() {
    if (!m_memViewer->IsCreated()) {
        m_memViewer->Create(m_hwnd, GetModuleHandleW(nullptr));
    }
    if (!m_memViewer->IsVisible()) {
        m_memViewer->Show();
    } else {
        m_memViewer->Hide();
    }
}

void App::GoToAddress(uintptr_t addr) {
    ToggleMemoryViewer();
    m_memViewer->GoToAddress(addr);
}

// ============================================================
// Input handlers
// ============================================================
void App::OnMouseDown(int x, int y, bool right, bool dbl) {
    m_ui.mouse = {x, y};
    if (!right) {
        m_ui.mouseDown = true;
        m_ui.mousePressed = true;
        m_ui.mouseDoubleClicked = dbl;
    } else {
        m_ui.mouseRightPressed = true;
    }
}

void App::OnMouseUp(int x, int y) {
    m_ui.mouse = {x, y};
    m_ui.mouseDown = false;
    m_ui.mouseReleased = true;
    m_ui.scrollDragId = -1;
}

void App::OnMouseMove(int x, int y) {
    m_ui.mouse = {x, y};
    if (m_ui.scrollDragId != -1) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::OnMouseWheel(int delta) {
    m_ui.mouseWheel = delta;
}

void App::OnKeyDown(WPARAM key) {
    m_ui.keyCode = (int)key;
    m_ui.keyPressed = true;
    m_ui.keyCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m_ui.keyShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // Escape defocuses the active text input
    if (key == VK_ESCAPE && m_ui.focusId != -1) {
        m_ui.focusId = -1;
        return;
    }

    HandleHotkeys();
}

void App::OnChar(wchar_t ch) {
    m_ui.charInput = ch;
    m_ui.hasCharInput = true;
}

void App::OnTimer() {
    m_ui.caretBlink = !m_ui.caretBlink;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::HandleHotkeys() {
    // Don't fire hotkeys when a text input is focused (IDs < 9000 are text
    // inputs; 9000+ are scrollbars/other non-text controls). This prevents
    // Ctrl+F/R/S/etc. from hijacking typing.
    if (m_ui.focusId != -1 && m_ui.focusId < 9000) {
        // Allow Escape to defocus (handled in OnKeyDown, but keep this
        // branch explicit so future callers don't bypass it).
        if (m_ui.keyPressed && m_ui.keyCode == VK_ESCAPE) {
            m_ui.focusId = -1;
        }
        return;
    }

    if (!m_ui.keyCtrl) return;
    switch (m_ui.keyCode) {
    case 'F':
        if (m_process.IsOpen() && !m_scanner.IsScanning()) {
            if (m_scanner.IsFirstScan()) DoNewScan();
            else DoNextScan();
        }
        break;
    case 'R': DoResetScan(); break;
    case 'O':
        m_processList = m_process.EnumerateProcesses();
        m_showProcessPicker = true;
        break;
    case 'S': m_table.Save("miku_table.mwt"); break;
    case 'M': ToggleMemoryViewer(); break;
    case 'G': ToggleMemoryViewer(); break;
    }
}
