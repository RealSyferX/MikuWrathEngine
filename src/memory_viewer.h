#pragma once
#include "types.h"
#include "process_manager.h"
#include "disassembler.h"
#include "ui.h"
#include <vector>
#include <functional>

class MemoryViewer {
public:
    MemoryViewer();
    ~MemoryViewer();

    void Create(HWND parent, HINSTANCE hInst);
    void Show();
    void Hide();
    bool IsVisible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    bool IsCreated() const { return m_hwnd != nullptr; }

    void SetProcess(ProcessManager* pm) { m_pm = pm; }
    void SetDisassembler(Disassembler* dis) { m_dis = dis; }
    void SetAddToTableCallback(std::function<void(uintptr_t, ValueType)> cb) { m_addToTable = cb; }

    void GoToAddress(uintptr_t addr);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    ProcessManager* m_pm = nullptr;
    Disassembler* m_dis = nullptr;
    std::function<void(uintptr_t, ValueType)> m_addToTable;

    UIContext m_ui;

    uintptr_t m_hexAddr = 0x00400000;
    uintptr_t m_disasmAddr = 0x00400000;
    char m_addrBuf[32] = "00400000";

    int m_hexLines = 16;
    int m_hexCols = 16;
    std::vector<DisasmInstruction> m_disasmView;
    uintptr_t m_selectedAddr = 0;

    bool m_addrBarFocus = false;
    UINT_PTR m_caretTimer = 0;
    bool m_caretBlink = true;

    char m_patchBuf[256] = {};
    bool m_showPatchPopup = false;
    uintptr_t m_patchAddr = 0;
    size_t m_patchMaxLen = 0;

    void OnPaint(Gdiplus::Graphics* g, int w, int h);
    void OnMouseDown(int x, int y, bool right, bool dbl);
    void OnMouseWheel(int delta);
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM key);

    void RenderAddrBar(Gdiplus::Graphics* g, RECT& rc);
    void RenderDisasm(Gdiplus::Graphics* g, RECT& rc);
    void RenderHex(Gdiplus::Graphics* g, RECT& rc);
    void RenderPatchPopup(Gdiplus::Graphics* g, RECT& rc);

    void RefreshDisasm();
    void ScrollHex(int lines);
    void ScrollDisasm(int lines);
    void DoNOP(uintptr_t addr, size_t len);
    void DoPatch(uintptr_t addr, const char* hexStr);
    void ParseAndGo();
    void ShowContextMenu(int x, int y, uintptr_t addr, bool isDisasm, size_t instSize);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static const wchar_t* ClassName() { return L"MikuMemViewer"; }
};
