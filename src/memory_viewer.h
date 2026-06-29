#pragma once
#include "types.h"
#include "process_manager.h"
#include "disassembler.h"
#include <functional>
#include <vector>

class MemoryViewer {
public:
    void SetProcess(ProcessManager* pm) { m_pm = pm; }
    void SetDisassembler(Disassembler* dis) { m_dis = dis; }

    void SetAddToTableCallback(std::function<void(uintptr_t, ValueType)> cb) {
        m_addToTable = cb;
    }

    void Render();
    void GoToAddress(uintptr_t addr);

private:
    ProcessManager* m_pm = nullptr;
    Disassembler* m_dis = nullptr;
    std::function<void(uintptr_t, ValueType)> m_addToTable;

    uintptr_t m_hexAddr = 0x00400000;
    uintptr_t m_disasmAddr = 0x00400000;
    char m_addrBuf[32] = "00400000";

    int m_hexLines = 16;
    int m_hexCols = 16;

    std::vector<DisasmInstruction> m_disasmView;
    uintptr_t m_selectedAddr = 0;

    bool m_addrBarFocus = false;

    void RenderAddressBar();
    void RenderHexView();
    void RenderDisasmView();
    void RefreshDisasm();
    void ScrollHex(int lines);
    void ScrollDisasm(int lines);
    void ParseAndGo();
};
