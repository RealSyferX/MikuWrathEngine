#pragma once
#include "types.h"
#include "process_manager.h"
#include "scanner.h"
#include "disassembler.h"
#include "memory_viewer.h"
#include "address_table.h"
#include <vector>

class App {
public:
    App();
    ~App();

    void Render();
    void SetHwnd(HWND h) { m_hwnd = h; }
    bool TakePendingDrag() { bool v = m_pendingDrag; m_pendingDrag = false; return v; }

private:
    HWND m_hwnd = nullptr;
    bool m_pendingDrag = false;
    bool m_alwaysOnTop = true;

    ProcessManager m_process;
    Scanner m_scanner;
    Disassembler m_disasm;
    MemoryViewer m_memViewer;
    AddressTable m_table;

    int m_typeIdx = 2;        // 4 Bytes default
    int m_scanTypeIdx = 0;    // Exact Value default
    int m_nextScanTypeIdx = 0;
    char m_valueBuf[256] = {};
    char m_valueBuf2[256] = {};
    bool m_hexMode = false;
    bool m_writableOnly = true;

    bool m_showMemViewer = false;
    bool m_showProcessPicker = false;
    int m_selectedResult = -1;

    std::vector<ProcessInfo> m_processList;
    char m_processFilter[256] = {};

    float m_updateTimer = 0.0f;

    void RenderTitleBar();
    void RenderMenuBar();
    void RenderProcessBar();
    void RenderScanPanel();
    void RenderResults();
    void RenderProcessPicker();

    void DoNewScan();
    void DoNextScan();
    void DoResetScan();

    ValueType CurrentType() const { return (ValueType)m_typeIdx; }

    static bool Stristr(const std::string& haystack, const std::string& needle);
};
