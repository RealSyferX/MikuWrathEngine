#include "app.h"
#include "memory_viewer.h"
#include "access_hits_window.h"
#include "value_utils.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <commdlg.h>

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
    m_settings.Load();
    UI::RecreateFonts(m_settings.fontSize);
    m_scanModuleItems[0] = "All";

    m_scanner.SetProcess(&m_process);
    m_debugger.SetProcessManager(&m_process);
    m_memViewer = std::make_unique<MemoryViewer>();
    m_memViewer->SetProcess(&m_process);
    m_memViewer->SetDisassembler(&m_disasm);
    m_memViewer->SetAddToTableCallback([this](uintptr_t addr, ValueType type) {
        m_table.Add(addr, type);
    });
    m_accessHitsWindow = std::make_unique<AccessHitsWindow>();
    m_accessHitsWindow->SetDebugger(&m_debugger);
    m_accessHitsWindow->SetDisassembler(&m_disasm);
    m_accessHitsWindow->SetProcessManager(&m_process);
    m_accessHitsWindow->SetGoToCallback([this](uintptr_t addr) {
        GoToAddress(addr);
    });
    m_lastTick = GetTickCount();
}

App::~App() {
    m_debugger.Detach();
    m_process.CloseTarget();
}

// ============================================================
// File dialogs
// ============================================================
std::string App::PickSaveFile(const char* defaultName) {
    char buf[MAX_PATH] = {};
    strncpy(buf, defaultName, MAX_PATH - 1);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = "MikuWrath Table (*.mwt)\0*.mwt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "mwt";
    if (GetSaveFileNameA(&ofn)) return buf;
    return "";
}

std::string App::PickOpenFile() {
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = "MikuWrath Table (*.mwt)\0*.mwt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) return buf;
    return "";
}

// ============================================================
// Layout
// ============================================================
void App::ComputeLayout(int w, int h) {
    int fontAdj = std::max(0, UI::g_fontSize - 9) * 2;
    int y = 0;
    m_layout.titleBar = { 0, y, w, y + Theme::TITLE_H + fontAdj }; y += Theme::TITLE_H + fontAdj;
    m_layout.menuBar = { 0, y, w, y + Theme::MENU_H + fontAdj }; y += Theme::MENU_H + fontAdj;
    m_layout.processBar = { 0, y, w, y + 28 + fontAdj * 2 }; y += 28 + fontAdj * 2;

    // Scan panel height is dynamic: when the window is narrow the Region
    // combo wraps onto a third row, so the panel needs more vertical space.
    int scanH = (w < 700 ? 80 : 52) + fontAdj * 4;
    m_layout.scanPanel = { 0, y, w, y + scanH }; y += scanH;

    // Remaining vertical space is split between results and the address
    // table.  Enforce a minimum per-panel height so that short windows still
    // show something usable; if there is not enough room the panels extend
    // past the bottom edge rather than collapsing to zero.
    int remain = h - y;
    int minPanel = 60;
    if (remain < minPanel * 2) remain = minPanel * 2;
    int resultsH = remain / 2;
    m_layout.resultsPanel = { 0, y, w, y + resultsH }; y += resultsH;
    m_layout.tablePanel = { 0, y, w, std::max(y + minPanel, h) };
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
        int rowH = std::max(16, UI::g_fontSize + 8);
        int visibleRows = std::max<int>(1, (m_layout.tablePanel.bottom - m_layout.tablePanel.top - 48) / rowH);
        m_table.UpdateValues(m_process, m_table.GetScrollPos(), visibleRows);
        m_updateTimer = 0.0f;
    }

    // Detect scan completion and auto-populate results
    bool isScanning = m_scanner.IsScanning();
    if (m_wasScanning && !isScanning) {
        m_results = m_scanner.GetResultsCopy();
        m_cachedResultValues.clear();
    }
    m_wasScanning = isScanning;

    // Auto-attach: poll for the target process every 2 seconds
    static float autoAttachTimer = 0.0f;
    if (!m_process.IsOpen() && m_settings.autoAttach[0]) {
        autoAttachTimer += m_dt;
        if (autoAttachTimer >= 2.0f) {
            autoAttachTimer = 0.0f;
            auto procs = m_process.EnumerateProcesses();
            for (auto& p : procs) {
                if (_stricmp(p.name.c_str(), m_settings.autoAttach) == 0) {
                    if (m_process.OpenTarget(p.pid)) {
                        m_disasm.Init(m_process.Is64Bit());
                        m_scanner.Reset();
                        m_scanner.SetScanRange(0, 0);
                        m_scanModuleIdx = 0;
                        m_cachedModules = m_process.EnumerateModules();
                        m_cachedRegions.clear();
                        m_cachedResultValues.clear();
                        m_cachedResultScroll = -1;
                        m_results.clear();
                        m_regionCacheTimer = 2.0f;
                        m_moduleCacheTimer = 0.0f;
                    }
                    break;
                }
            }
        }
    } else {
        autoAttachTimer = 0.0f;
    }
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

    // Manual add-address modal overlay
    if (m_showAddDialog) {
        RenderAddDialog();
    }

    // Settings modal overlay
    if (m_showSettings) {
        RenderSettings();
    }

    // Debugger overlays
    if (m_showBreakpoints) {
        RenderBreakpoints();
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
            m_pendingMenu = i;
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
    int btnCount = m_process.IsOpen() ? 4 : 3;
    int totalBtnW = btnW * btnCount + 4 * (btnCount - 1);
    // When the window is narrow, shrink the buttons so they always fit
    // alongside the process info text (leave at least 200px on the left).
    if (totalBtnW > rc.right - 200) {
        btnW = std::max(56, (int)(rc.right - 200 - 4 * (btnCount - 1)) / btnCount);
        totalBtnW = btnW * btnCount + 4 * (btnCount - 1);
    }
    int bx = rc.right - totalBtnW - 8;

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
            m_debugger.Detach();
            m_process.CloseTarget();
            m_scanner.Reset();
            m_scanner.SetScanRange(0, 0);
            m_scanModuleIdx = 0;
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

    int w = m_ui.width;
    int y1 = rc.top + 4;    // row 1
    int y2 = rc.top + 30;   // row 2
    bool canScan = m_process.IsOpen() && !m_scanner.IsScanning();
    bool isFirst = m_scanner.IsFirstScan();

    // ---- Result count (always visible, right-aligned on row 1) ----
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
    int cw = 0;
    UI::MeasureText(m_ui.g, countStr, nullptr, &cw, nullptr);
    int countX = w - cw - 8;
    if (countX < 8) countX = 8; // never go off the left edge
    UI::DrawText(m_ui.g, countX, y1 + 3, countStr, Theme::CLR_YELLOW());
    // Right boundary for row 1 controls (leave room for the result count)
    int row1Right = countX - 8;

    // ---- Row 1: Type, ScanType, Hex, Writable, Region ----
    const char* typeNames[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","String","AOB"};
    UI::DrawText(m_ui.g, 8, y1+3, "Type:", Theme::CLR_TEXT());
    UI::ComboBox(m_ui, 20, {48, y1, 148, y1+22}, typeNames, 8, &m_typeIdx);

    // Scan type combo
    if (isFirst) {
        const char* scanTypes[] = {"Exact Value","Bigger Than","Smaller Than","Between","Unknown Init"};
        UI::DrawText(m_ui.g, 158, y1+3, "Scan:", Theme::CLR_TEXT());
        UI::ComboBox(m_ui, 200, {198, y1, 318, y1+22}, scanTypes, 5, &m_scanTypeIdx);
    } else {
        const char* nextTypes[] = {"Exact Value","Bigger Than","Smaller Than","Between",
                                    "Changed","Unchanged","Increased","Decreased"};
        UI::DrawText(m_ui.g, 158, y1+3, "Next:", Theme::CLR_TEXT());
        UI::ComboBox(m_ui, 201, {198, y1, 318, y1+22}, nextTypes, 8, &m_nextScanTypeIdx);
    }

    // Hex + Writable checkboxes
    int cx = 328;
    if (CurrentType() <= ValueType::Qword) {
        if (cx + 50 < row1Right) {
            UI::Checkbox(m_ui, 21, {cx, y1, cx+50, y1+22}, "Hex", &m_hexMode);
            cx += 55;
        }
    }
    if (cx + 100 < row1Right) {
        UI::Checkbox(m_ui, 22, {cx, y1, cx+100, y1+22}, "Writable", &m_writableOnly);
        cx += 105;
    }

    // Region combo — restrict scan to a specific module (or All)
    // Refresh module cache periodically so the combo stays current
    m_moduleCacheTimer += m_dt;
    if (m_cachedModules.empty() || m_moduleCacheTimer >= 2.0f) {
        if (m_process.IsOpen()) {
            m_cachedModules = m_process.EnumerateModules();
        }
        m_moduleCacheTimer = 0.0f;
    }
    // Rebuild the persistent const-char* array from cached module names
    m_scanModuleItems[0] = "All";
    m_scanModuleCount = 1;
    for (size_t i = 0; i < m_cachedModules.size() && m_scanModuleCount < 64; i++) {
        m_scanModuleItems[m_scanModuleCount++] = m_cachedModules[i].name.c_str();
    }
    if (m_scanModuleIdx >= m_scanModuleCount) m_scanModuleIdx = 0;

    bool regionOnRow3 = false;
    if (cx + 200 < row1Right) {
        // Region fits on row 1
        UI::DrawText(m_ui.g, cx, y1+3, "Region:", Theme::CLR_TEXT());
        UI::ComboBox(m_ui, 28, {cx + 50, y1, cx + 200, y1+22},
                     m_scanModuleItems, m_scanModuleCount, &m_scanModuleIdx);
    } else {
        // Region wraps to row 3 (panel is 70px tall when w < 700)
        regionOnRow3 = true;
    }

    // ---- Row 2: Value input, Scan/Stop button, Reset ----
    bool unknownInit = (isFirst && m_scanTypeIdx == 4);
    bool isBetween = (isFirst ? m_scanTypeIdx == 3 : m_nextScanTypeIdx == 3);
    bool needValue = !unknownInit && !(isFirst ? false : m_nextScanTypeIdx >= 4);

    UI::DrawText(m_ui.g, 8, y2+3, "Value:", Theme::CLR_TEXT());

    // Scan button is always anchored to the right edge so it stays visible.
    int scanBtnW = 100;
    int scanBtnX = w - scanBtnW - 8;
    // Reset button sits to the left of the scan button (next scans only)
    int resetBtnW = 60;
    int resetBtnX = scanBtnX - resetBtnW - 4;
    // Value input right boundary must not overlap the buttons
    int valRight = isFirst ? (scanBtnX - 8) : (resetBtnX - 8);
    int valLeft = 48;
    int valW = valRight - valLeft;
    if (valW > 200) valW = 200; // cap at original width
    if (valW < 60) valW = 60;   // minimum usable width

    if (!needValue) {
        // Draw disabled input
        RECT inpRc = {valLeft, y2, valLeft + valW, y2+22};
        UI::FillRect(m_ui.g, inpRc, Theme::BG_CONTROL());
        UI::DrawRect(m_ui.g, inpRc, Theme::BORDER());
    } else {
        const char* hint = (CurrentType() == ValueType::AOB) ? "e.g. 7F ?? 90 41" : nullptr;
        if (hint) {
            if (m_valueBuf[0] == '\0') {
                UI::DrawText(m_ui.g, valLeft + 4, y2+3, hint, Theme::CLR_DIM());
            }
        }
        UI::TextInput(m_ui, 23, {valLeft, y2, valLeft + valW, y2+22}, m_valueBuf, sizeof(m_valueBuf));
    }

    if (isBetween && needValue) {
        int val2Left = valLeft + valW + 10;
        int val2MaxW = resetBtnX - val2Left - 4;
        int val2W = std::min(200, val2MaxW);
        if (val2W < 40) val2W = 40; // minimum usable, may overlap when extremely narrow
        UI::DrawText(m_ui.g, val2Left - 14, y2+3, "to", Theme::CLR_TEXT());
        UI::TextInput(m_ui, 24, {val2Left, y2, val2Left + val2W, y2+22}, m_valueBuf2, sizeof(m_valueBuf2));
    }

    // Scan / Stop / disabled button (right-anchored)
    if (m_scanner.IsScanning()) {
        // Stop button (red)
        RECT btnRc = {scanBtnX, y2, scanBtnX+scanBtnW, y2+22};
        UI::FillRect(m_ui.g, btnRc, Gdiplus::Color(180, 30, 30));
        UI::DrawRect(m_ui.g, btnRc, Gdiplus::Color(220, 50, 50));
        UI::DrawText(m_ui.g, scanBtnX+30, y2+3, "Stop", Theme::CLR_TEXT());
        if (m_ui.PtInRect(btnRc) && m_ui.mousePressed) {
            m_scanner.RequestCancel();
        }
        // Progress bar to the left of the stop button
        int pbRight = scanBtnX - 8;
        int pbLeft = valLeft + valW + 10;
        if (pbLeft < pbRight) {
            RECT pbRc = {pbLeft, y2, pbRight, y2+22};
            UI::ProgressBar(m_ui, pbRc, m_scanner.GetProgress());
        }
    } else if (!canScan) {
        // Draw disabled button
        RECT btnRc = {scanBtnX, y2, scanBtnX+scanBtnW, y2+22};
        UI::FillRect(m_ui.g, btnRc, Theme::BG_CONTROL());
        UI::DrawRect(m_ui.g, btnRc, Theme::BORDER());
        UI::DrawText(m_ui.g, scanBtnX+20, y2+3, isFirst ? "First Scan" : "Next Scan", Theme::CLR_DIM());
    } else {
        if (isFirst) {
            if (UI::Button(m_ui, 25, {scanBtnX, y2, scanBtnX+scanBtnW, y2+22}, "First Scan")) DoNewScan();
        } else {
            if (UI::Button(m_ui, 26, {scanBtnX, y2, scanBtnX+scanBtnW, y2+22}, "Next Scan")) DoNextScan();
            if (UI::Button(m_ui, 27, {resetBtnX, y2, resetBtnX+resetBtnW, y2+22}, "Reset")) DoResetScan();
        }
    }

    // ---- Row 3: Region combo (only when wrapped) ----
    if (regionOnRow3) {
        int y3 = rc.top + 56;
        UI::DrawText(m_ui.g, 8, y3+3, "Region:", Theme::CLR_TEXT());
        int regW = std::min(200, w - 58 - 8);
        UI::ComboBox(m_ui, 28, {58, y3, 58 + regW, y3+22},
                     m_scanModuleItems, m_scanModuleCount, &m_scanModuleIdx);
    }
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

    // Column headers — value column position is proportional to panel width
    int colAddr = 4;
    int colVal = std::min(180, std::max(80, (int)(rc.right - rc.left) / 3));
    UI::FillRect(m_ui.g, {rc.left, rc.top, rc.right, rc.top + 18}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, rc.left + colAddr, rc.top + 2, "Address", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colVal, rc.top + 2, "Value", Theme::CLR_DIM());
    UI::DrawSeparator(m_ui.g, rc.top + 18, rc.left, rc.right);

    // Rows
    int rowH = std::max(16, UI::g_fontSize + 7);
    int yStart = rc.top + 20;
    int visibleRows = std::max<int>(0, (rc.bottom - yStart) / rowH);

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
    if (visibleRows > 0 && displayCount > (size_t)visibleRows) {
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
        m_showAddDialog = true;
    }
    if (UI::Button(m_ui, 31, {78, ty, 144, ty+20}, "Clear All")) m_table.Clear();
    if (UI::Button(m_ui, 32, {152, ty, 200, ty+20}, "Save")) {
        m_pendingSaveTable = true;
    }
    if (UI::Button(m_ui, 33, {208, ty, 256, ty+20}, "Load")) {
        m_pendingLoadTable = true;
    }

    UI::DrawText(m_ui.g, 270, ty+2, "Right-click row for options", Theme::CLR_DIM());

    UI::DrawSeparator(m_ui.g, ty + 22, rc.left, rc.right);

    // Column headers — widths are proportional to panel width so columns
    // never overflow.  The remove ("X") button is anchored to the right edge
    // and is therefore always visible.
    int hy = ty + 24;
    int colFz = 4;
    int colX = (int)rc.right - 24;
    int availW = colX - (int)rc.left;
    int colDesc = 24;
    int colAddr = std::max(colDesc + 60, availW / 4);
    int colType = std::max(colAddr + 50, availW / 2);
    int colVal = std::max(colType + 40, availW * 3 / 4);
    if (colVal > availW - 30) colVal = availW - 30; // keep value column usable
    UI::FillRect(m_ui.g, {rc.left, hy, rc.right, hy + 18}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, rc.left + colFz, hy + 2, "Fz", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colDesc, hy + 2, "Description", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colAddr, hy + 2, "Address", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colType, hy + 2, "Type", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, rc.left + colVal, hy + 2, "Value", Theme::CLR_DIM());

    // Rows
    int rowH = std::max(20, UI::g_fontSize + 8);
    int yStart = hy + 20;
    auto& entries = m_table.Entries();
    int visibleRows = std::max<int>(0, (rc.bottom - yStart) / rowH);
    int maxScroll = std::max(0, (int)entries.size() - visibleRows);
    m_table.ClampScroll(maxScroll);

    if (m_ui.PtInRect(rc) && m_ui.mouseWheel != 0) {
        m_table.SetScrollPos(m_table.GetScrollPos() - m_ui.mouseWheel / 120 * 3);
        m_table.ClampScroll(maxScroll);
    }

    const char* typeNames[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","String","AOB"};

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_table.GetScrollPos() + i;
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

        // Value (display only — refreshed by UpdateValues() every 250ms)
        bool valChanged = UI::TextInput(m_ui, 1003 + idx * 6,
            {rc.left + colVal, y, colX - 2, y + rowH}, e.editValue, sizeof(e.editValue));
        e.isEditing = (m_ui.focusId == 1003 + idx * 6);
        if (valChanged) {
            if (m_ui.keyPressed && m_ui.keyCode == VK_RETURN) {
                ::WriteValueString(m_process, e.address, e.type, e.editValue);
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

// ============================================================
// Manual add-address dialog (modal overlay)
// ============================================================
void App::RenderAddDialog() {
    int w = m_ui.width, h = m_ui.height;
    // Dim background
    UI::FillRect(m_ui.g, {0, 0, w, h}, Gdiplus::Color(128, 0, 0, 0));

    // Clamp dialog size to the window so it never goes off-screen.
    int dlgW = std::min(400, std::max(300, w - 20));
    int dlgH = std::min(200, std::max(160, h - 20));
    RECT dlg = {w/2 - dlgW/2, h/2 - dlgH/2, w/2 + dlgW/2, h/2 + dlgH/2};
    UI::FillRect(m_ui.g, dlg, Theme::BG_PANEL());
    UI::DrawNeonBorder(m_ui.g, dlg.left, dlg.top, dlg.right-dlg.left-1, dlg.bottom-dlg.top-1, Theme::NEON());

    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 8, "Add Address Manually", Theme::CLR_TEXT());

    // Address field
    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 32, "Address:", Theme::CLR_TEXT());
    if (m_addAddrBuf[0] == '\0' && m_ui.focusId != 6000) {
        UI::DrawText(m_ui.g, dlg.left + 82, dlg.top + 33, "0x00400000 or module+offset", Theme::CLR_DIM());
    }
    UI::TextInput(m_ui, 6000, {dlg.left + 80, dlg.top + 30, dlg.right - 10, dlg.top + 52}, m_addAddrBuf, sizeof(m_addAddrBuf));

    // Type field
    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 58, "Type:", Theme::CLR_TEXT());
    const char* typeNames[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","String","AOB"};
    UI::ComboBox(m_ui, 6001, {dlg.left + 80, dlg.top + 56, dlg.left + 180, dlg.top + 78}, typeNames, 8, &m_addTypeIdx);

    // Description field
    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 84, "Desc:", Theme::CLR_TEXT());
    UI::TextInput(m_ui, 6002, {dlg.left + 80, dlg.top + 82, dlg.right - 10, dlg.top + 104}, m_addDescBuf, sizeof(m_addDescBuf));

    // OK / Cancel buttons — anchored to the dialog bottom so they stay
    // visible even when the dialog height is clamped.
    int btnY = dlg.bottom - 30;
    if (UI::Button(m_ui, 6003, {dlg.right - 180, btnY, dlg.right - 100, btnY + 22}, "OK")) {
        uintptr_t addr = 0;
        std::string addrStr = m_addAddrBuf;
        // Check for module+offset
        size_t plusPos = addrStr.find('+');
        if (plusPos != std::string::npos) {
            std::string modName = addrStr.substr(0, plusPos);
            std::string offsetStr = addrStr.substr(plusPos + 1);
            uintptr_t base = m_process.GetModuleBase(modName.c_str());
            uintptr_t offset = strtoull(offsetStr.c_str(), nullptr, 0);
            addr = base + offset;
        } else {
            addr = strtoull(addrStr.c_str(), nullptr, 16);
        }
        m_table.Add(addr, (ValueType)m_addTypeIdx, m_addDescBuf);
        m_showAddDialog = false;
        m_addAddrBuf[0] = '\0';
        m_addDescBuf[0] = '\0';
        m_ui.focusId = -1;
    }
    // Cancel button
    if (UI::Button(m_ui, 6004, {dlg.right - 90, btnY, dlg.right - 10, btnY + 22}, "Cancel")) {
        m_showAddDialog = false;
        m_ui.focusId = -1;
    }
}

// ============================================================
// Settings dialog (modal overlay)
// ============================================================
void App::RenderSettings() {
    int w = m_ui.width, h = m_ui.height;
    // Dim background
    UI::FillRect(m_ui.g, {0, 0, w, h}, Gdiplus::Color(128, 0, 0, 0));

    // Clamp dialog size to the window so it never goes off-screen.
    int dlgW = std::min(400, std::max(300, w - 20));
    int dlgH = std::min(280, std::max(200, h - 20));
    RECT dlg = {w/2 - dlgW/2, h/2 - dlgH/2, w/2 + dlgW/2, h/2 + dlgH/2};
    UI::FillRect(m_ui.g, dlg, Theme::BG_PANEL());
    UI::DrawNeonBorder(m_ui.g, dlg.left, dlg.top, dlg.right-dlg.left-1, dlg.bottom-dlg.top-1, Theme::NEON());

    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 8, "Settings", Theme::CLR_TEXT());

    // Auto-attach
    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 35, "Auto Attach:", Theme::CLR_TEXT());
    if (m_settings.autoAttach[0] == '\0' && m_ui.focusId != 7000) {
        UI::DrawText(m_ui.g, dlg.left + 100, dlg.top + 36, "e.g. MAT.exe (empty=off)", Theme::CLR_DIM());
    }
    UI::TextInput(m_ui, 7000, {dlg.left + 100, dlg.top + 33, dlg.right - 10, dlg.top + 55},
                  m_settings.autoAttach, sizeof(m_settings.autoAttach));

    // Font size
    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 62, "Font Size:", Theme::CLR_TEXT());
    char sizeStr[8]; snprintf(sizeStr, sizeof(sizeStr), "%d", m_settings.fontSize);
    UI::DrawText(m_ui.g, dlg.left + 100, dlg.top + 63, sizeStr, Theme::CLR_TEXT());

    RECT minusBtn = {dlg.left + 130, dlg.top + 60, dlg.left + 150, dlg.top + 82};
    if (UI::Button(m_ui, 7001, minusBtn, "-")) {
        if (m_settings.fontSize > 6) {
            m_settings.fontSize--;
            UI::RecreateFonts(m_settings.fontSize);
        }
    }
    RECT plusBtn = {dlg.left + 155, dlg.top + 60, dlg.left + 175, dlg.top + 82};
    if (UI::Button(m_ui, 7002, plusBtn, "+")) {
        if (m_settings.fontSize < 20) {
            m_settings.fontSize++;
            UI::RecreateFonts(m_settings.fontSize);
        }
    }

    // Debugger type
    UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 88, "Debugger:", Theme::CLR_TEXT());
    const char* dbgTypes[] = {"None", "VEH", "Windows"};
    int dbgType = m_settings.debuggerType;
    UI::ComboBox(m_ui, 7005, {dlg.left + 100, dlg.top + 86, dlg.left + 200, dlg.top + 108}, dbgTypes, 3, &dbgType);
    m_settings.debuggerType = dbgType;

    // Auto-attach status
    if (m_process.IsOpen()) {
        UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 116, "Status: Attached", Theme::CLR_GREEN());
    } else if (m_settings.autoAttach[0]) {
        UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 116, "Status: Will auto-attach on next launch", Theme::CLR_YELLOW());
    } else {
        UI::DrawText(m_ui.g, dlg.left + 10, dlg.top + 116, "Status: No auto-attach configured", Theme::CLR_DIM());
    }

    // Save & Close — anchored to the dialog bottom so they stay visible
    // even when the dialog height is clamped to a short window.
    int btnY = dlg.bottom - 30;
    RECT saveBtn = {dlg.right - 190, btnY, dlg.right - 100, btnY + 22};
    if (UI::Button(m_ui, 7003, saveBtn, "Save")) {
        m_settings.Save();
        m_showSettings = false;
        m_ui.focusId = -1;
    }
    RECT closeBtn = {dlg.right - 90, btnY, dlg.right - 10, btnY + 22};
    if (UI::Button(m_ui, 7004, closeBtn, "Close")) {
        m_showSettings = false;
        m_ui.focusId = -1;
    }
}

// ============================================================
// Breakpoints overlay
// ============================================================
void App::RenderBreakpoints() {
    int w = m_ui.width, h = m_ui.height;
    UI::FillRect(m_ui.g, {0, 0, w, h}, Theme::BG_MAIN());
    UI::DrawNeonBorder(m_ui.g, 0, 0, w-1, h-1, Theme::NEON());

    UI::FillRect(m_ui.g, {0, 0, w, 30}, Theme::BG_TITLE());
    UI::DrawText(m_ui.g, 8, 7, "Breakpoints", Theme::CLR_TEXT());

    if (UI::Button(m_ui, 8000, {w - 80, 3, w - 10, 27}, "Back")) {
        m_showBreakpoints = false;
    }

    // Attach/Detach button
    if (!m_debugger.IsAttached()) {
        if (UI::Button(m_ui, 8001, {8, 35, 120, 57}, "Attach")) {
            m_debugger.SetType((DebuggerType)m_settings.debuggerType);
            m_debugger.Attach();
        }
    } else {
        if (UI::Button(m_ui, 8002, {8, 35, 120, 57}, "Detach")) {
            m_debugger.Detach();
        }
        UI::DrawText(m_ui.g, 130, 38, "Attached", Theme::CLR_GREEN());
    }

    UI::DrawSeparator(m_ui.g, 60, 2, w - 2);

    // Breakpoint list
    auto bps = m_debugger.GetBreakpoints();
    int rowH = std::max(16, UI::g_fontSize + 7);
    int y = 65;

    // Headers
    UI::FillRect(m_ui.g, {0, y, w, y + rowH}, Theme::BG_CONTROL());
    UI::DrawText(m_ui.g, 8, y + 2, "Address", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 180, y + 2, "Type", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 260, y + 2, "Size", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 320, y + 2, "HW/INT3", Theme::CLR_DIM());
    UI::DrawText(m_ui.g, 400, y + 2, "Label", Theme::CLR_DIM());

    y += rowH + 2;

    for (size_t i = 0; i < bps.size(); i++) {
        auto& bp = bps[i];
        int ry = y + (int)i * rowH;
        if (ry >= h - 20) break;

        UI::FillRect(m_ui.g, {0, ry, w, ry + rowH}, i % 2 ? Theme::BG_MAIN() : Theme::BG_PANEL());

        char buf[32];
        snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)bp.address);
        UI::DrawText(m_ui.g, 8, ry + 1, buf, Theme::CLR_BLUE());

        const char* typeStr = bp.type == BreakType::Execute ? "Execute" :
                              bp.type == BreakType::Write ? "Write" : "Access";
        UI::DrawText(m_ui.g, 180, ry + 1, typeStr, Theme::CLR_TEXT());

        snprintf(buf, sizeof(buf), "%zu", bp.size);
        UI::DrawText(m_ui.g, 260, ry + 1, buf, Theme::CLR_TEXT());

        UI::DrawText(m_ui.g, 320, ry + 1, bp.hardware ? "HW" : "INT3", Theme::CLR_YELLOW());
        UI::DrawText(m_ui.g, 400, ry + 1, bp.label, Theme::CLR_TEXT());
    }

    if (bps.empty()) {
        UI::DrawText(m_ui.g, 8, y + 4, "No breakpoints set.", Theme::CLR_DIM());
    }

    // Clear all button
    if (UI::Button(m_ui, 8003, {130, 35, 230, 57}, "Clear All")) {
        m_debugger.ClearAllBreakpoints();
    }

    // -------------------------------------------------------
    // Register view + stepping controls (shown when halted)
    // -------------------------------------------------------
    if (m_debugger.IsHalted()) {
        auto ctx = m_debugger.GetLastContext();
        int regY = y + (int)bps.size() * rowH + 10;

        UI::DrawText(m_ui.g, 8, regY, "=== HALTED ===", Theme::CLR_RED());
        regY += 20;

        // Register grid — 4 columns
        struct RegInfo { const char* name; uint64_t value; };
        RegInfo regs[] = {
            {"RAX", ctx.rax}, {"RBX", ctx.rbx}, {"RCX", ctx.rcx}, {"RDX", ctx.rdx},
            {"RSI", ctx.rsi}, {"RDI", ctx.rdi}, {"RSP", ctx.rsp}, {"RBP", ctx.rbp},
            {"R8 ", ctx.r8},  {"R9 ", ctx.r9},  {"R10", ctx.r10}, {"R11", ctx.r11},
            {"R12", ctx.r12}, {"R13", ctx.r13}, {"R14", ctx.r14}, {"R15", ctx.r15},
        };

        int colW = (w - 16) / 4;
        for (int i = 0; i < 16; i++) {
            int col = i % 4;
            int row = i / 4;
            int rx = 8 + col * colW;
            int ry = regY + row * 18;

            char buf[40];
            snprintf(buf, sizeof(buf), "%s=%016llX", regs[i].name, (unsigned long long)regs[i].value);
            UI::DrawText(m_ui.g, rx, ry, buf, Theme::CLR_BLUE());
        }

        regY += (16 / 4 + 1) * 18;

        char ripBuf[40];
        snprintf(ripBuf, sizeof(ripBuf), "RIP=%016llX  EFlags=%08X",
                 (unsigned long long)ctx.rip, ctx.eflags);
        UI::DrawText(m_ui.g, 8, regY, ripBuf, Theme::CLR_YELLOW());
        regY += 20;

        // Step/Continue buttons
        if (UI::Button(m_ui, 8100, {8, regY, 108, regY + 24}, "Step Into (F11)")) {
            m_debugger.StepInto();
        }
        if (UI::Button(m_ui, 8101, {115, regY, 215, regY + 24}, "Continue (F5)")) {
            m_debugger.Continue();
        }
    }
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
    int rowH = std::max(18, UI::g_fontSize + 7);
    int yStart = 60;
    int visibleRows = std::max<int>(0, (h - yStart - 30) / rowH);
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
                m_scanner.SetScanRange(0, 0);
                m_scanModuleIdx = 0;
                m_results.clear();
                // Caches belong to the previous process; invalidate everything.
                m_cachedResultValues.clear();
                m_cachedResultScroll = -1;
                m_cachedRegions.clear();
                m_cachedModules.clear();
                m_regionCacheTimer = 2.0f;
                m_moduleCacheTimer = 0.0f;
                m_showProcessPicker = false;
            }
        }
    }

    // Scrollbar
    if (visibleRows > 0 && filtered.size() > (size_t)visibleRows) {
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
    int rowH = std::max(18, UI::g_fontSize + 7);
    int yStart = 35;
    int visibleRows = std::max<int>(0, (h - yStart - 10) / rowH);

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
    if (visibleRows > 0 && regions.size() > (size_t)visibleRows) {
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
    int rowH = std::max(18, UI::g_fontSize + 7);
    int yStart = 35;
    int visibleRows = std::max<int>(0, (h - yStart - 10) / rowH);

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
            m_pendingCtxMenu = 3;
            m_pendingCtxX = m_ui.mouse.x;
            m_pendingCtxY = m_ui.mouse.y;
            m_pendingCtxAddr = m.base;
        }
    }

    // Scrollbar
    if (visibleRows > 0 && mods.size() > (size_t)visibleRows) {
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
    m_pendingCtxMenu = 1;
    m_pendingCtxX = x;
    m_pendingCtxY = y;
    m_pendingCtxAddr = addr;
}

void App::ShowTableContextMenu(int x, int y, size_t entryIdx) {
    m_pendingCtxMenu = 2;
    m_pendingCtxX = x;
    m_pendingCtxY = y;
    m_pendingCtxEntryIdx = entryIdx;
}

// ============================================================
// Actions
// ============================================================
void App::DoNewScan() {
    // Set scan range based on selected module
    if (m_scanModuleIdx > 0 && m_scanModuleIdx < m_scanModuleCount) {
        int modIdx = m_scanModuleIdx - 1;
        if (modIdx < (int)m_cachedModules.size()) {
            m_scanner.SetScanRange(m_cachedModules[modIdx].base, m_cachedModules[modIdx].size);
        }
    } else {
        m_scanner.SetScanRange(0, 0); // All regions
    }

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
    m_pendingToggleMemViewer = true;
}

void App::GoToAddress(uintptr_t addr) {
    m_pendingGoTo = true;
    m_pendingGoToAddr = addr;
    m_pendingToggleMemViewer = true;
}

// ============================================================
// Process pending deferred actions (called from message loop
// after DispatchMessage returns — safe to run modal loops here)
// ============================================================
void App::ProcessPendingActions() {
    // Pending combo box popup
    if (m_ui.pendingComboId != -1) {
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < m_ui.pendingComboCount; i++) {
            AppendMenuA(menu, MF_STRING | (i == m_ui.pendingComboSelected ? MF_CHECKED : 0),
                i + 1, m_ui.pendingComboItems[i]);
        }
        POINT pt = { m_ui.pendingComboRc.left, m_ui.pendingComboRc.bottom };
        ClientToScreen(m_hwnd, &pt);
        int result = (int)TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        if (result > 0 && m_ui.pendingComboSelectedPtr) {
            *m_ui.pendingComboSelectedPtr = result - 1;
        }
        m_ui.pendingComboId = -1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Pending menu bar popup
    if (m_pendingMenu >= 0) {
        int i = m_pendingMenu;
        m_pendingMenu = -1;
        HMENU menu = CreatePopupMenu();
        POINT pt = {0, 0};
        if (i == 0) {
            AppendMenuA(menu, MF_STRING, 101, "Save Table...");
            AppendMenuA(menu, MF_STRING, 102, "Load Table...");
            AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(menu, MF_STRING, 103, "Exit");
        } else if (i == 1) {
            AppendMenuA(menu, MF_STRING, 201, "Memory Viewer\tCtrl+M");
            AppendMenuA(menu, MF_STRING, 202, "Memory Regions");
            AppendMenuA(menu, MF_STRING, 203, "Module List");
            AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(menu, MF_STRING, 205, "Breakpoints");
            AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(menu, MF_STRING, 204, "Settings...");
        } else {
            AppendMenuA(menu, MF_STRING, 301, "About MikuWrathEngine");
            AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(menu, MF_STRING, 302, "v2.0 - GDI+ Edition");
        }
        // Position at menu button
        pt.x = 4 + i * 50;  // approximate
        pt.y = Theme::TITLE_H + Theme::MENU_H;
        ClientToScreen(m_hwnd, &pt);
        int result = (int)TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        switch (result) {
        case 101: m_pendingSaveTable = true; break;
        case 102: m_pendingLoadTable = true; break;
        case 103: PostQuitMessage(0); break;
        case 201: m_pendingToggleMemViewer = true; break;
        case 202: m_showRegionList = true; m_cachedRegions.clear(); m_regionCacheTimer = 2.0f; break;
        case 203: m_showModuleList = true; m_cachedModules.clear(); m_moduleCacheTimer = 2.0f; break;
        case 204: m_showSettings = true; break;
        case 205: m_showBreakpoints = true; break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Pending file save dialog
    if (m_pendingSaveTable) {
        m_pendingSaveTable = false;
        auto path = PickSaveFile("miku_table.mwt");
        if (!path.empty()) m_table.Save(path.c_str());
    }

    // Pending file load dialog
    if (m_pendingLoadTable) {
        m_pendingLoadTable = false;
        auto path = PickOpenFile();
        if (!path.empty()) m_table.Load(path.c_str());
    }

    // Pending context menu (results)
    if (m_pendingCtxMenu == 1) {
        m_pendingCtxMenu = 0;
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, "Add to Address Table");
        AppendMenuA(menu, MF_STRING, 2, "Browse Memory Region");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING, 3, "Copy Address");
        POINT pt = {m_pendingCtxX, m_pendingCtxY};
        ClientToScreen(m_hwnd, &pt);
        int res = (int)TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        switch (res) {
        case 1: m_table.Add(m_pendingCtxAddr, m_scanner.GetValueType()); break;
        case 2: m_pendingGoTo = true; m_pendingGoToAddr = m_pendingCtxAddr; m_pendingToggleMemViewer = true; break;
        case 3: { char b[32]; snprintf(b,sizeof(b),"0x%llX",(unsigned long long)m_pendingCtxAddr); CopyToClipboard(m_hwnd, b); } break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Pending context menu (table)
    if (m_pendingCtxMenu == 2) {
        m_pendingCtxMenu = 0;
        auto& entries = m_table.Entries();
        size_t idx = m_pendingCtxEntryIdx;
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, "Browse in Memory Viewer");
        AppendMenuA(menu, MF_STRING, 2, "Copy Address");
        AppendMenuA(menu, MF_STRING, 3, "Copy Value");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING, 5, "Find what accesses this address");
        AppendMenuA(menu, MF_STRING, 6, "Find what writes to this address");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING, 7, "Set Breakpoint (Execute)");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING, 4, "Delete Entry");
        POINT pt = {m_pendingCtxX, m_pendingCtxY};
        ClientToScreen(m_hwnd, &pt);
        int res = (int)TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        if (idx < entries.size()) {
            switch (res) {
            case 1: m_pendingGoTo = true; m_pendingGoToAddr = entries[idx].address; m_pendingToggleMemViewer = true; break;
            case 2: { char b[32]; snprintf(b,sizeof(b),"0x%llX",(unsigned long long)entries[idx].address); CopyToClipboard(m_hwnd, b); } break;
            case 3: CopyToClipboard(m_hwnd, entries[idx].editValue); break;
            case 4: m_table.Remove(idx); break;
            case 5: { // Find accesses
                uintptr_t addr = entries[idx].address;
                size_t sz = ValueTypeSize(entries[idx].type);
                if (sz == 0) sz = 4;
                m_debugger.SetType((DebuggerType)m_settings.debuggerType);
                if (m_debugger.StartFindAccesses(addr, sz)) {
                    if (!m_accessHitsWindow->IsCreated()) {
                        m_accessHitsWindow->Create(m_hwnd, GetModuleHandleW(nullptr));
                    }
                    char title[64];
                    snprintf(title, sizeof(title), "Find what accesses 0x%llX",
                             (unsigned long long)addr);
                    m_accessHitsWindow->SetTitle(title);
                    m_accessHitsWindow->SetAccessType(true);
                    m_accessHitsWindow->Show();
                }
            } break;
            case 6: { // Find writes
                uintptr_t addr = entries[idx].address;
                size_t sz = ValueTypeSize(entries[idx].type);
                if (sz == 0) sz = 4;
                m_debugger.SetType((DebuggerType)m_settings.debuggerType);
                if (m_debugger.StartFindWrites(addr, sz)) {
                    if (!m_accessHitsWindow->IsCreated()) {
                        m_accessHitsWindow->Create(m_hwnd, GetModuleHandleW(nullptr));
                    }
                    char title[64];
                    snprintf(title, sizeof(title), "Find what writes 0x%llX",
                             (unsigned long long)addr);
                    m_accessHitsWindow->SetTitle(title);
                    m_accessHitsWindow->SetAccessType(false);
                    m_accessHitsWindow->Show();
                }
            } break;
            case 7: { // Set breakpoint
                m_debugger.SetType((DebuggerType)m_settings.debuggerType);
                if (!m_debugger.IsAttached()) m_debugger.Attach();
                m_debugger.AddBreakpoint(entries[idx].address, BreakType::Execute, 1, entries[idx].description);
                m_showBreakpoints = true;
            } break;
            }
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Pending context menu (module list)
    if (m_pendingCtxMenu == 3) {
        m_pendingCtxMenu = 0;
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, "Browse in Memory Viewer");
        AppendMenuA(menu, MF_STRING, 2, "Copy Base Address");
        POINT pt = {m_pendingCtxX, m_pendingCtxY};
        ClientToScreen(m_hwnd, &pt);
        int res = (int)TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        switch (res) {
        case 1: m_pendingGoTo = true; m_pendingGoToAddr = m_pendingCtxAddr; m_pendingToggleMemViewer = true; m_showModuleList = false; break;
        case 2: { char b[32]; snprintf(b,sizeof(b),"0x%llX",(unsigned long long)m_pendingCtxAddr); CopyToClipboard(m_hwnd, b); } break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // Pending memory viewer toggle
    if (m_pendingToggleMemViewer) {
        m_pendingToggleMemViewer = false;
        if (!m_memViewer->IsCreated()) {
            m_memViewer->Create(m_hwnd, GetModuleHandleW(nullptr));
        }
        if (!m_memViewer->IsVisible()) {
            m_memViewer->Show();
        } else if (!m_pendingGoTo) {
            m_memViewer->Hide();
        }
    }

    // Pending go to address
    if (m_pendingGoTo) {
        m_pendingGoTo = false;
        m_memViewer->GoToAddress(m_pendingGoToAddr);
    }

    // Process memory viewer pending actions
    if (m_memViewer && m_memViewer->IsVisible()) {
        m_memViewer->ProcessPendingActions();
    }

    // Process access hits window pending actions
    if (m_accessHitsWindow && m_accessHitsWindow->IsVisible()) {
        m_accessHitsWindow->ProcessPendingActions();
        InvalidateRect(m_accessHitsWindow->GetHwnd(), nullptr, FALSE);
    }
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

    // Debugger stepping hotkeys (no Ctrl required) — only active when
    // the breakpoints view is open and the target is halted.
    if (m_showBreakpoints && m_debugger.IsHalted()) {
        if (m_ui.keyPressed) {
            if (m_ui.keyCode == VK_F5) m_debugger.Continue();
            else if (m_ui.keyCode == VK_F11) m_debugger.StepInto();
        }
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
    case 'S': m_pendingSaveTable = true; break;
    case 'M': ToggleMemoryViewer(); break;
    case 'G': ToggleMemoryViewer(); break;
    }
}
