#pragma once
#include "types.h"
#include "process_manager.h"
#include "disassembler.h"
#include "debugger.h"
#include "ui.h"
#include <vector>
#include <map>
#include <functional>
#include <string>

class AccessHitsWindow {
public:
    AccessHitsWindow();
    ~AccessHitsWindow();

    void Create(HWND parent, HINSTANCE hInst);
    void Show();
    void Hide();
    bool IsVisible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    bool IsCreated() const { return m_hwnd != nullptr; }
    HWND GetHwnd() const { return m_hwnd; }

    void SetDebugger(Debugger* dbg) { m_debugger = dbg; }
    void SetDisassembler(Disassembler* dis) { m_dis = dis; }
    void SetProcessManager(ProcessManager* pm) { m_pm = pm; }
    void SetTitle(const char* title);
    void SetAccessType(bool isAccess) { m_isAccess = isAccess; }
    void SetGoToCallback(std::function<void(uintptr_t)> cb) { m_goToCallback = cb; }

    void ProcessPendingActions();
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    Debugger* m_debugger = nullptr;
    Disassembler* m_dis = nullptr;
    ProcessManager* m_pm = nullptr;

    UIContext m_ui;
    UINT_PTR m_caretTimer = 0;
    bool m_caretBlink = true;
    bool m_isAccess = true;
    char m_title[64] = "Find what accesses";

    int m_scrollPos = 0;

    // Cache of disassembled instruction text per instruction address.
    // Cleared when the window is hidden so stale entries don't linger.
    std::map<uintptr_t, std::string> m_disasmCache;
    std::function<void(uintptr_t)> m_goToCallback;

    void OnPaint(Gdiplus::Graphics* g, int w, int h);
    void OnMouseWheel(int delta);
    void OnMouseDown(int x, int y, bool right, bool dbl);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static const wchar_t* ClassName() { return L"MikuAccessHits"; }
};
