#pragma once
#include "types.h"
#include "process_manager.h"
#include "scanner.h"
#include "disassembler.h"
#include "address_table.h"
#include "ui.h"
#include <vector>
#include <memory>
#include <functional>

class MemoryViewer;

class App {
public:
    App();
    ~App();

    void SetHwnd(HWND h) { m_hwnd = h; }
    HWND GetHwnd() { return m_hwnd; }
    bool TakePendingDrag() { bool v = m_pendingDrag; m_pendingDrag = false; return v; }
    UIContext m_ui;

    void OnPaint(Gdiplus::Graphics* g);
    void OnMouseDown(int x, int y, bool rightClick, bool doubleClick);
    void OnMouseUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int delta);
    void OnKeyDown(WPARAM key);
    void OnChar(wchar_t ch);
    void OnTimer();
    void HandleHotkeys();

    void ToggleMemoryViewer();
    void GoToAddress(uintptr_t addr);

    ProcessManager& GetProcess() { return m_process; }
    Disassembler& GetDisasm() { return m_disasm; }
    AddressTable& GetTable() { return m_table; }

private:
    HWND m_hwnd = nullptr;
    bool m_pendingDrag = false;
    bool m_alwaysOnTop = true;

    ProcessManager m_process;
    Scanner m_scanner;
    Disassembler m_disasm;
    AddressTable m_table;
    std::unique_ptr<MemoryViewer> m_memViewer;

    int m_typeIdx = 2;
    int m_scanTypeIdx = 0;
    int m_nextScanTypeIdx = 0;
    char m_valueBuf[256] = {};
    char m_valueBuf2[256] = {};
    bool m_hexMode = false;
    bool m_writableOnly = true;

    std::vector<uintptr_t> m_results;
    int m_resultsScroll = 0;
    int m_selectedResult = -1;
    bool m_wasScanning = false;

    bool m_showProcessPicker = false;
    std::vector<ProcessInfo> m_processList;
    char m_processFilter[256] = {};
    int m_processScroll = 0;
    int m_selectedProcess = -1;

    bool m_showRegionList = false;
    bool m_showModuleList = false;
    int m_regionScroll = 0;
    int m_moduleScroll = 0;

    // Cached enumeration results (refreshed on a timer, not every frame)
    std::vector<MemoryRegion> m_cachedRegions;
    std::vector<ProcessManager::ModuleInfo> m_cachedModules;
    float m_regionCacheTimer = 0.0f;
    float m_moduleCacheTimer = 0.0f;

    // Cached result value strings (batched reads instead of per-row RPM)
    std::vector<std::string> m_cachedResultValues;
    float m_resultValueTimer = 0.0f;
    int m_cachedResultScroll = -1; // invalidate when scroll changes

    int m_menuOpen = -1;

    float m_dt = 0.0f;
    DWORD m_lastTick = 0;
    float m_updateTimer = 0.0f;

    struct Layout {
        RECT titleBar, menuBar, processBar, scanPanel;
        RECT resultsPanel, tablePanel;
    } m_layout;

    void ComputeLayout(int w, int h);
    void UpdateState();

    void RenderTitleBar();
    void RenderMenuBar();
    void RenderProcessBar();
    void RenderScanPanel();
    void RenderResults();
    void RenderAddressTable();
    void RenderRegionList();
    void RenderModuleList();
    void RenderProcessPicker();

    void DoNewScan();
    void DoNextScan();
    void DoResetScan();
    void WriteValueStr(AddressEntry& e);

    ValueType CurrentType() const { return (ValueType)m_typeIdx; }
    static bool Stristr(const std::string& haystack, const std::string& needle);

    void ShowResultContextMenu(int x, int y, uintptr_t addr);
    void ShowTableContextMenu(int x, int y, size_t entryIdx);
};
